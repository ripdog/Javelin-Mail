#include "gui/shell/EmailContextMenuEditorDialog.h"

#include "gui/settings/GuiSettings.h"
#include "gui/shell/EmailContextMenuLayout.h"

#include <KActionCollection>
#include <KActionSelector>
#include <KLocalizedString>

#include <QAction>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace javelin::gui::shell
{
    namespace
    {
        constexpr auto actionIdRole = Qt::UserRole;

        [[nodiscard]] QString displayText(const QAction& action)
        {
            auto text = action.text();
            text.remove(QLatin1Char('&'));
            return text;
        }
    } // namespace

    EmailContextMenuEditorDialog::EmailContextMenuEditorDialog(
        javelin::gui::settings::GuiSettings& settings, KActionCollection& actions, QWidget* parent)
        : QDialog(parent), m_settings(settings), m_actions(actions)
    {
        setWindowTitle(i18n("Configure Email Context Menu"));
        resize(760, 480);
        auto* layout = new QVBoxLayout(this);
        m_selector = new KActionSelector(this);
        m_selector->setObjectName(QStringLiteral("emailContextMenuActionSelector"));
        m_selector->setAvailableLabel(i18n("Available actions:"));
        m_selector->setSelectedLabel(i18n("Current actions:"));
        m_selector->setAvailableInsertionPolicy(KActionSelector::Sorted);
        layout->addWidget(m_selector, 1);

        auto* separatorRow = new QHBoxLayout;
        auto* addSeparator = new QPushButton(i18n("Add Separator"), this);
        addSeparator->setObjectName(QStringLiteral("addEmailContextMenuSeparator"));
        separatorRow->addStretch(1);
        separatorRow->addWidget(addSeparator);
        layout->addLayout(separatorRow);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
                                                 QDialogButtonBox::RestoreDefaults,
                                             this);
        layout->addWidget(buttons);
        connect(addSeparator, &QPushButton::clicked, this,
                &EmailContextMenuEditorDialog::addSeparator);
        connect(m_selector, &KActionSelector::removed, this,
                [this](QListWidgetItem* item)
                {
                    if (item->data(actionIdRole).toString() != emailContextMenuSeparatorId())
                        return;
                    delete m_selector->availableListWidget()->takeItem(
                        m_selector->availableListWidget()->row(item));
                });
        connect(buttons, &QDialogButtonBox::accepted, this, &EmailContextMenuEditorDialog::save);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this,
                [this] { populate(defaultEmailContextMenuLayout()); });

        populate(
            effectiveEmailContextMenuLayout(m_settings.workspaceSettings().emailContextMenuLayout));
    }

    QListWidgetItem* EmailContextMenuEditorDialog::itemForId(const QString& id) const
    {
        if (id == emailContextMenuSeparatorId())
        {
            auto* item = new QListWidgetItem(i18nc("context menu separator", "— Separator —"));
            item->setData(actionIdRole, id);
            return item;
        }
        auto* action = m_actions.action(id);
        if (action == nullptr)
            return nullptr;
        auto* item = new QListWidgetItem(action->icon(), displayText(*action));
        item->setData(actionIdRole, id);
        return item;
    }

    void EmailContextMenuEditorDialog::populate(const std::vector<QString>& layout)
    {
        auto* available = m_selector->availableListWidget();
        auto* selected = m_selector->selectedListWidget();
        available->clear();
        selected->clear();

        const auto normalized = normalizeEmailContextMenuLayout(layout);
        for (const auto& id : normalized)
        {
            if (auto* item = itemForId(id); item != nullptr)
                selected->addItem(item);
        }
        for (const auto& id : supportedEmailContextMenuActionIds())
        {
            if (std::ranges::contains(normalized, id))
                continue;
            if (auto* item = itemForId(id); item != nullptr)
                available->addItem(item);
        }
        available->sortItems();
        if (selected->count() > 0)
            selected->setCurrentRow(0);
    }

    void EmailContextMenuEditorDialog::addSeparator()
    {
        auto* selected = m_selector->selectedListWidget();
        auto* item = itemForId(emailContextMenuSeparatorId());
        const int currentRow = selected->currentRow();
        selected->insertItem(currentRow < 0 ? selected->count() : currentRow + 1, item);
        selected->setCurrentItem(item);
    }

    std::vector<QString> EmailContextMenuEditorDialog::selectedLayout() const
    {
        const auto* selected = m_selector->selectedListWidget();
        std::vector<QString> layout;
        layout.reserve(static_cast<std::size_t>(selected->count()));
        for (int row = 0; row < selected->count(); ++row)
            layout.push_back(selected->item(row)->data(actionIdRole).toString());
        return normalizeEmailContextMenuLayout(layout);
    }

    void EmailContextMenuEditorDialog::save()
    {
        const auto selected = selectedLayout();
        if (selected.empty())
        {
            QMessageBox::warning(this, windowTitle(),
                                 i18n("The context menu must contain at least one action."));
            return;
        }
        auto layout = emailContextMenuOverrideForLayout(selected);
        auto workspace = m_settings.workspaceSettings();
        workspace.emailContextMenuLayout = std::move(layout);
        if (const auto error = m_settings.updateWorkspace(std::move(workspace)))
        {
            QMessageBox::warning(
                this, windowTitle(),
                i18n("The email context menu could not be saved.\n\n%1", error->detail));
            return;
        }
        accept();
    }
} // namespace javelin::gui::shell
