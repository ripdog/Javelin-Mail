#include "gui/shell/MailExportController.h"

#include "app/MailExportApplicationPorts.h"
#include "gui/settings/GuiSettings.h"
#include "jmap/cache/AccountReadRepository.h"
#include "jmap/cache/MailboxReadRepository.h"

#include <KLocalizedString>

#include <QCoroTask>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <optional>
#include <ranges>
#include <utility>
#include <variant>
#include <vector>

namespace javelin::gui::shell
{
    namespace
    {
        struct MailExportDialogChoice
        {
            javelin::app::MailExportFormat format = javelin::app::MailExportFormat::Eml;
            QString destinationDirectory;
        };

        [[nodiscard]] std::optional<MailExportDialogChoice>
        promptMailExport(QWidget& parent, const QStringView displayName, const bool accountScope)
        {
            QDialog dialog{&parent};
            dialog.setWindowTitle(accountScope ? i18n("Export Account") : i18n("Export Mailbox"));
            auto* layout = new QVBoxLayout(&dialog);

            auto* summary = new QLabel(
                accountScope
                    ? i18n("Export all readable mailboxes in “%1”, including hidden mailboxes.",
                           displayName.toString())
                    : i18n("Export mailbox “%1”.", displayName.toString()),
                &dialog);
            summary->setWordWrap(true);
            layout->addWidget(summary);

            auto* form = new QFormLayout;
            auto* format = new QComboBox(&dialog);
            format->addItem(i18n("EML files"));
            format->addItem(i18n("mbox files"));
            form->addRow(i18n("Format:"), format);

            auto* destinationRow = new QWidget(&dialog);
            auto* destinationLayout = new QHBoxLayout(destinationRow);
            destinationLayout->setContentsMargins(0, 0, 0, 0);
            auto* destination = new QLineEdit(destinationRow);
            destination->setPlaceholderText(i18n("Choose a new or empty folder"));
            auto* browse = new QPushButton(QIcon::fromTheme(QStringLiteral("document-open-folder")),
                                           i18n("Browse…"), destinationRow);
            destinationLayout->addWidget(destination, 1);
            destinationLayout->addWidget(browse);
            form->addRow(i18n("Destination:"), destinationRow);
            layout->addLayout(form);

            auto* note = new QLabel(
                i18n("Messages that are not stored locally will be downloaded as needed. Large "
                     "exports continue in Task Center if this window is closed."),
                &dialog);
            note->setWordWrap(true);
            layout->addWidget(note);

            auto* buttons =
                new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
            buttons->button(QDialogButtonBox::Save)->setText(i18n("Export"));
            buttons->button(QDialogButtonBox::Save)->setEnabled(false);
            layout->addWidget(buttons);

            const auto updateExportEnabled = [destination, buttons]
            {
                buttons->button(QDialogButtonBox::Save)
                    ->setEnabled(!destination->text().trimmed().isEmpty());
            };
            QObject::connect(destination, &QLineEdit::textChanged, &dialog,
                             [updateExportEnabled](const QString&) { updateExportEnabled(); });
            QObject::connect(browse, &QPushButton::clicked, &dialog,
                             [&dialog, destination]
                             {
                                 const QString selected = QFileDialog::getExistingDirectory(
                                     &dialog, i18n("Export Destination"),
                                     destination->text().trimmed().isEmpty()
                                         ? QDir::homePath()
                                         : destination->text().trimmed(),
                                     QFileDialog::ShowDirsOnly);
                                 if (!selected.isEmpty())
                                     destination->setText(selected);
                             });
            QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
            QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

            if (dialog.exec() != QDialog::Accepted)
                return std::nullopt;
            return MailExportDialogChoice{
                .format = format->currentIndex() == 0 ? javelin::app::MailExportFormat::Eml
                                                      : javelin::app::MailExportFormat::MboxRd,
                .destinationDirectory = destination->text().trimmed(),
            };
        }
    } // namespace

    MailExportController::MailExportController(javelin::app::MailExportPort& exportPort,
                                               javelin::jmap::cache::AccountReader& accountReader,
                                               javelin::jmap::cache::MailboxReader& mailboxReader,
                                               javelin::gui::settings::GuiSettings& settings,
                                               QWidget& dialogParent, QObject* parent)
        : QObject(parent), m_exportPort(exportPort), m_accountReader(accountReader),
          m_mailboxReader(mailboxReader), m_settings(settings), m_dialogParent(dialogParent)
    {
    }

    void MailExportController::exportMailbox(std::string accountId, std::string mailboxId)
    {
        const auto mailboxesResult = m_mailboxReader.listMailboxTree(accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&mailboxesResult))
        {
            Q_EMIT statusMessage(error->message, 10000);
            return;
        }
        const auto& mailboxes =
            std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(mailboxesResult);
        const auto mailbox =
            std::ranges::find(mailboxes, mailboxId, &javelin::jmap::cache::MailboxTreeItem::id);
        if (mailbox == mailboxes.end() || mailbox->pendingCreate || !mailbox->myRights.mayReadItems)
        {
            Q_EMIT statusMessage(i18n("The mailbox is not available for export."), 5000);
            return;
        }
        startExport(std::move(accountId), std::move(mailboxId),
                    QString::fromStdString(mailbox->name), false);
    }

    void MailExportController::exportAccount(std::string accountId)
    {
        QString displayName =
            m_settings.accountForCachedId(QString::fromStdString(accountId)).displayName;
        if (displayName.isEmpty())
        {
            const auto accountResult = m_accountReader.findById(accountId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&accountResult))
            {
                Q_EMIT statusMessage(error->message, 10000);
                return;
            }
            const auto& account =
                std::get<std::optional<javelin::jmap::cache::CachedAccount>>(accountResult);
            if (account.has_value())
                displayName = QString::fromStdString(account->name);
        }
        if (displayName.isEmpty())
            displayName = i18n("Unnamed account");
        startExport(std::move(accountId), {}, std::move(displayName), true);
    }

    void MailExportController::startExport(std::string accountId, std::string mailboxId,
                                           QString displayName, const bool accountScope)
    {
        const auto choice = promptMailExport(m_dialogParent, displayName, accountScope);
        if (!choice.has_value())
            return;
        auto task = m_exportPort.startExport({
            .accountId = std::move(accountId),
            .scopeKind = accountScope ? javelin::app::MailExportScopeKind::Account
                                      : javelin::app::MailExportScopeKind::Mailbox,
            .mailboxId = accountScope ? std::optional<std::string>{std::nullopt}
                                      : std::optional<std::string>{std::move(mailboxId)},
            .format = choice->format,
            .destinationDirectory = choice->destinationDirectory,
        });
        QCoro::connect(
            std::move(task), this,
            [this](javelin::app::MailExportStartResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    Q_EMIT operationFailed(*error);
                    return;
                }
                Q_EMIT statusMessage(
                    i18n("Mail export started. Progress is available in Task Center."), 5000);
            });
    }
} // namespace javelin::gui::shell
