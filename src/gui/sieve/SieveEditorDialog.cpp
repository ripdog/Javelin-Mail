#include "gui/sieve/SieveEditorDialog.h"

#include "app/SieveApplicationPorts.h"

#include <QCoroTask>

#include <KLocalizedString>
#include <KTextEditor/Document>
#include <KTextEditor/Editor>
#include <KTextEditor/View>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>

namespace javelin::gui::sieve
{
    SieveEditorDialog::SieveEditorDialog(javelin::app::SieveCommandPort& commandPort,
                                         std::string ownerAccountId, QWidget* parent)
        : QDialog(parent), m_commandPort(commandPort), m_ownerAccountId(std::move(ownerAccountId))
    {
        setWindowTitle(i18n("Sieve Rules"));
        resize(960, 640);

        auto* layout = new QVBoxLayout(this);
        auto* splitter = new QSplitter(this);
        m_scriptList = new QListWidget(splitter);
        m_scriptList->setMinimumWidth(220);

        m_document = KTextEditor::Editor::instance()->createDocument(this);
        m_document->setHighlightingMode(QStringLiteral("Sieve"));
        auto* view = m_document->createView(splitter);
        splitter->addWidget(m_scriptList);
        splitter->addWidget(view);
        splitter->setStretchFactor(1, 1);
        layout->addWidget(splitter, 1);

        auto* footer = new QHBoxLayout;
        m_newButton = new QPushButton(i18nc("@action:button", "New"), this);
        m_deleteButton = new QPushButton(i18nc("@action:button", "Delete"), this);
        footer->addWidget(m_newButton);
        footer->addWidget(m_deleteButton);
        m_activeCheckBox = new QCheckBox(i18nc("@option:check", "Active"), this);
        footer->addWidget(m_activeCheckBox);
        m_statusLabel = new QLabel(i18n("Loading scripts…"), this);
        m_statusLabel->setWordWrap(true);
        footer->addWidget(m_statusLabel, 1);
        m_validateButton = new QPushButton(i18nc("@action:button", "Validate"), this);
        m_saveButton = new QPushButton(i18nc("@action:button", "Save"), this);
        footer->addWidget(m_validateButton);
        footer->addWidget(m_saveButton);
        layout->addLayout(footer);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);

        connect(m_scriptList, &QListWidget::currentRowChanged, this,
                &SieveEditorDialog::selectScript);
        connect(m_document, &KTextEditor::Document::textChanged, this,
                [this]
                {
                    if (m_loaded)
                    {
                        m_dirty = true;
                        m_statusLabel->setText(i18n("Not saved"));
                        updateActions();
                    }
                });
        connect(m_validateButton, &QPushButton::clicked, this, &SieveEditorDialog::validateScript);
        connect(m_saveButton, &QPushButton::clicked, this, &SieveEditorDialog::saveScript);
        connect(m_newButton, &QPushButton::clicked, this, &SieveEditorDialog::newScript);
        connect(m_deleteButton, &QPushButton::clicked, this, &SieveEditorDialog::deleteScript);
        connect(m_activeCheckBox, &QCheckBox::toggled, this, &SieveEditorDialog::setScriptActive);
        updateActions();
        loadScripts();
    }

    void SieveEditorDialog::loadScripts()
    {
        std::string selectedId;
        if (m_currentRow >= 0 && m_currentRow < static_cast<int>(m_scripts.size()))
            selectedId = m_scripts[static_cast<std::size_t>(m_currentRow)].id;
        setBusy(true);
        auto task = m_commandPort.requestSieveScripts(m_ownerAccountId);
        QCoro::connect(
            std::move(task), this,
            [this, selectedId = std::move(selectedId)](javelin::jmap::sieve::SieveListResult result)
            {
                setBusy(false);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    showError(*error);
                    return;
                }
                m_scripts =
                    std::get<std::vector<javelin::jmap::sieve::SieveScript>>(std::move(result));
                {
                    const QSignalBlocker blocker{m_scriptList};
                    m_scriptList->clear();
                    for (const auto& script : m_scripts)
                    {
                        auto title = QString::fromStdString(script.name);
                        if (script.isActive)
                            title += i18nc("@item suffix marking active Sieve script", " (active)");
                        m_scriptList->addItem(title);
                    }
                }
                m_currentRow = -1;
                m_loaded = false;
                m_dirty = false;
                m_document->setReadWrite(true);
                m_document->setText(QString{});
                if (m_scripts.empty())
                {
                    const QSignalBlocker blocker{m_activeCheckBox};
                    m_activeCheckBox->setChecked(false);
                    m_statusLabel->setText(i18n("No Sieve scripts."));
                }
                else
                {
                    auto selectedRow = 0;
                    if (!selectedId.empty())
                    {
                        const auto selected = std::find_if(m_scripts.cbegin(), m_scripts.cend(),
                                                           [&selectedId](const auto& script)
                                                           { return script.id == selectedId; });
                        if (selected != m_scripts.cend())
                            selectedRow =
                                static_cast<int>(std::distance(m_scripts.cbegin(), selected));
                    }
                    m_scriptList->setCurrentRow(selectedRow);
                }
                updateActions();
            });
    }

    void SieveEditorDialog::selectScript(const int row)
    {
        if (row < 0 || row >= static_cast<int>(m_scripts.size()) || row == m_currentRow)
            return;
        if (m_dirty)
        {
            const auto answer =
                QMessageBox::question(this, i18n("Discard Changes?"),
                                      i18n("This script has unsaved changes. Discard them?"));
            if (answer != QMessageBox::Yes)
            {
                const QSignalBlocker blocker{m_scriptList};
                m_scriptList->setCurrentRow(m_currentRow);
                return;
            }
        }

        m_currentRow = row;
        {
            const QSignalBlocker blocker{m_activeCheckBox};
            m_activeCheckBox->setChecked(m_scripts[static_cast<std::size_t>(row)].isActive);
        }
        m_loaded = false;
        m_dirty = false;
        m_document->setReadWrite(true);
        m_document->setText(QString{});
        if (m_scripts[static_cast<std::size_t>(row)].id.empty())
        {
            m_loaded = true;
            m_dirty = true;
            m_statusLabel->setText(i18n("New script — not saved"));
            updateActions();
            return;
        }
        setBusy(true);
        m_statusLabel->setText(i18n("Loading script…"));
        auto task = m_commandPort.requestSieveScript(m_ownerAccountId,
                                                     m_scripts[static_cast<std::size_t>(row)]);
        QCoro::connect(
            std::move(task), this,
            [this, row](javelin::jmap::sieve::SieveContentResult result)
            {
                setBusy(false);
                if (row != m_currentRow)
                    return;
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    showError(*error);
                    return;
                }
                m_document->setReadWrite(true);
                m_document->setText(QString::fromUtf8(std::get<QByteArray>(std::move(result))));
                m_loaded = true;
                m_dirty = false;
                m_statusLabel->setText(i18n("Ready"));
                updateActions();
            });
    }

    void SieveEditorDialog::newScript()
    {
        if (m_dirty &&
            QMessageBox::question(this, i18n("Discard Changes?"),
                                  i18n("The current script has unsaved changes. Discard them?")) !=
                QMessageBox::Yes)
            return;
        bool accepted = false;
        const auto name =
            QInputDialog::getText(this, i18n("New Sieve Script"), i18n("Script name:"),
                                  QLineEdit::Normal, {}, &accepted)
                .trimmed();
        if (!accepted || name.isEmpty())
            return;
        m_dirty = false;
        m_scripts.push_back(
            {.id = {}, .name = name.toStdString(), .blobId = {}, .isActive = false});
        m_scriptList->addItem(name);
        m_scriptList->setCurrentRow(static_cast<int>(m_scripts.size() - 1));
    }

    void SieveEditorDialog::deleteScript()
    {
        if (m_currentRow < 0)
            return;
        const auto script = m_scripts[static_cast<std::size_t>(m_currentRow)];
        if (QMessageBox::question(this, i18n("Delete Sieve Script?"),
                                  i18n("Delete “%1”? This cannot be undone.",
                                       QString::fromStdString(script.name))) != QMessageBox::Yes)
            return;
        if (script.id.empty())
        {
            removeCurrentScriptFromList();
            return;
        }
        setBusy(true);
        m_statusLabel->setText(script.isActive ? i18n("Deactivating and deleting…")
                                               : i18n("Deleting…"));
        auto task = m_commandPort.deleteSieveScript(m_ownerAccountId, script);
        QCoro::connect(std::move(task), this,
                       [this](javelin::jmap::sieve::SieveDeleteResult result)
                       {
                           setBusy(false);
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&result))
                           {
                               showError(*error);
                               return;
                           }
                           removeCurrentScriptFromList();
                       });
    }

    void SieveEditorDialog::setScriptActive(const bool active)
    {
        if (m_currentRow < 0 || m_busy)
            return;
        const auto script = m_scripts[static_cast<std::size_t>(m_currentRow)];
        if (script.id.empty() || script.isActive == active)
            return;
        setBusy(true);
        m_statusLabel->setText(active ? i18n("Activating…") : i18n("Deactivating…"));
        auto task = m_commandPort.setSieveScriptActive(m_ownerAccountId, script, active);
        QCoro::connect(
            std::move(task), this,
            [this, active,
             previous = script.isActive](javelin::jmap::sieve::SieveActivationResult result)
            {
                setBusy(false);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    const QSignalBlocker blocker{m_activeCheckBox};
                    m_activeCheckBox->setChecked(previous);
                    showError(*error);
                    return;
                }
                m_scripts[static_cast<std::size_t>(m_currentRow)].isActive = active;
                loadScripts();
            });
    }

    void SieveEditorDialog::removeCurrentScriptFromList()
    {
        const auto row = m_currentRow;
        m_currentRow = -1;
        m_loaded = false;
        m_dirty = false;
        m_scripts.erase(m_scripts.begin() + row);
        delete m_scriptList->takeItem(row);
        m_document->setReadWrite(true);
        m_document->setText(QString{});
        if (m_scripts.empty())
            m_statusLabel->setText(i18n("No Sieve scripts."));
        else
            m_scriptList->setCurrentRow(std::min(row, static_cast<int>(m_scripts.size() - 1)));
        updateActions();
    }

    void SieveEditorDialog::validateScript()
    {
        setBusy(true);
        m_statusLabel->setText(i18n("Validating…"));
        auto task =
            m_commandPort.validateSieveScript(m_ownerAccountId, m_document->text().toUtf8());
        QCoro::connect(
            std::move(task), this,
            [this](javelin::jmap::sieve::SieveValidationResult result)
            {
                setBusy(false);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    showError(*error);
                    return;
                }
                const auto& validation = std::get<javelin::jmap::sieve::SieveValidation>(result);
                m_statusLabel->setText(validation.message);
            });
    }

    void SieveEditorDialog::saveScript()
    {
        if (m_currentRow < 0)
            return;
        setBusy(true);
        m_statusLabel->setText(i18n("Validating…"));
        auto task = m_commandPort.saveSieveScript(m_ownerAccountId,
                                                  m_scripts[static_cast<std::size_t>(m_currentRow)],
                                                  m_document->text().toUtf8());
        QCoro::connect(
            std::move(task), this,
            [this](javelin::jmap::sieve::SieveSaveResult result)
            {
                setBusy(false);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    showError(*error);
                    return;
                }
                m_scripts[static_cast<std::size_t>(m_currentRow)] =
                    std::get<javelin::jmap::sieve::SieveScript>(std::move(result));
                auto title =
                    QString::fromStdString(m_scripts[static_cast<std::size_t>(m_currentRow)].name);
                if (m_scripts[static_cast<std::size_t>(m_currentRow)].isActive)
                    title += i18nc("@item suffix marking active Sieve script", " (active)");
                m_scriptList->item(m_currentRow)->setText(title);
                m_dirty = false;
                m_statusLabel->setText(i18n("Saved"));
                updateActions();
            });
    }

    void SieveEditorDialog::setBusy(const bool busy)
    {
        m_busy = busy;
        m_scriptList->setEnabled(!busy);
        updateActions();
    }

    void SieveEditorDialog::showError(const javelin::jmap::OperationError& error)
    {
        m_statusLabel->setText(error.code == javelin::jmap::OperationErrorCode::InvalidUserInput
                                   ? error.message
                                   : i18n("The operation failed."));
    }

    void SieveEditorDialog::updateActions()
    {
        m_document->setReadWrite(m_loaded && !m_busy);
        m_newButton->setEnabled(!m_busy);
        m_deleteButton->setEnabled(m_loaded && !m_busy);
        const bool canChangeActive = m_loaded && !m_dirty && !m_busy && m_currentRow >= 0 &&
                                     !m_scripts[static_cast<std::size_t>(m_currentRow)].id.empty();
        m_activeCheckBox->setEnabled(canChangeActive);
        m_validateButton->setEnabled(m_loaded && !m_busy);
        m_saveButton->setEnabled(m_loaded && m_dirty && !m_busy);
    }
} // namespace javelin::gui::sieve
