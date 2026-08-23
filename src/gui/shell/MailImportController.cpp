#include "gui/shell/MailImportController.h"

#include "app/MailImportApplicationPorts.h"
#include "jmap/cache/AccountReadRepository.h"
#include "jmap/cache/MailboxReadRepository.h"

#include <KLocalizedString>

#include <QCoroTask>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace javelin::gui::shell
{
    namespace
    {
        struct MailImportDialogChoice
        {
            std::string accountId;
            std::optional<std::string> mailboxId;
            bool recreateHierarchy = false;
        };

        [[nodiscard]] QString sourceSummary(const QStringList& paths)
        {
            if (paths.size() == 1)
            {
                const QFileInfo info{paths.front()};
                return info.isDir() ? i18n("Import directory “%1”.", info.fileName())
                                    : i18n("Import mail file “%1”.", info.fileName());
            }
            return i18np("Import %1 mail file.", "Import %1 mail files.", paths.size());
        }

        [[nodiscard]] QString
        mailboxPath(const javelin::jmap::cache::MailboxTreeItem& mailbox,
                    const std::unordered_map<std::string,
                                             const javelin::jmap::cache::MailboxTreeItem*>& byId)
        {
            QStringList components;
            std::unordered_set<std::string> visited;
            const auto* current = &mailbox;
            while (current != nullptr && visited.insert(current->id).second)
            {
                components.prepend(QString::fromStdString(current->name));
                if (!current->parentId.has_value())
                    break;
                const auto parent = byId.find(*current->parentId);
                current = parent == byId.end() ? nullptr : parent->second;
            }
            return components.join(QStringLiteral(" / "));
        }

        [[nodiscard]] std::variant<std::optional<MailImportDialogChoice>,
                                   javelin::jmap::cache::DatabaseError>
        promptMailImport(QWidget& parent, javelin::jmap::cache::AccountReader& accountReader,
                         javelin::jmap::cache::MailboxReader& mailboxReader,
                         const QStringList& sourcePaths,
                         const std::optional<std::string>& preferredAccountId,
                         const std::optional<std::string>& preferredMailboxId,
                         const bool hierarchySource, const bool hierarchyDefault)
        {
            const auto accountsResult = accountReader.listAll();
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&accountsResult))
                return *error;
            auto accounts =
                std::get<std::vector<javelin::jmap::cache::CachedAccount>>(accountsResult);
            std::erase_if(accounts, [](const auto& account)
                          { return !account.hasMailCapability || account.isReadOnly; });

            QDialog dialog{&parent};
            dialog.setWindowTitle(i18n("Import Mail"));
            auto* layout = new QVBoxLayout(&dialog);

            auto* summary = new QLabel(sourceSummary(sourcePaths), &dialog);
            summary->setWordWrap(true);
            layout->addWidget(summary);

            auto* form = new QFormLayout;
            auto* accountCombo = new QComboBox(&dialog);
            for (const auto& account : accounts)
            {
                const auto name = account.name.empty() ? QString::fromStdString(account.accountId)
                                                       : QString::fromStdString(account.name);
                accountCombo->addItem(name, QString::fromStdString(account.accountId));
                accountCombo->setItemData(accountCombo->count() - 1,
                                          account.mayCreateTopLevelMailbox, Qt::UserRole + 1);
            }
            form->addRow(i18n("Account:"), accountCombo);

            auto* mailboxCombo = new QComboBox(&dialog);
            form->addRow(hierarchySource ? i18n("Parent mailbox:") : i18n("Mailbox:"),
                         mailboxCombo);

            QCheckBox* hierarchy = nullptr;
            if (hierarchySource)
            {
                hierarchy = new QCheckBox(i18n("Recreate folder hierarchy"), &dialog);
                hierarchy->setChecked(hierarchyDefault);
                form->addRow(QString{}, hierarchy);
            }
            layout->addLayout(form);

            auto* duplicateInfo = new QLabel(
                i18n("Javelin imports the original message bytes. If the server reports an "
                     "existing equivalent message, Javelin reuses it and adds the requested "
                     "mailbox placement. Read, starred, and tag state are not restored."),
                &dialog);
            duplicateInfo->setWordWrap(true);
            layout->addWidget(duplicateInfo);

            auto* continuationInfo = new QLabel(
                i18n("Large imports continue in Task Center. Network or sign-in problems pause the "
                     "job and can be resumed safely."),
                &dialog);
            continuationInfo->setWordWrap(true);
            layout->addWidget(continuationInfo);

            auto* buttons =
                new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, &dialog);
            buttons->button(QDialogButtonBox::Open)->setText(i18n("Import"));
            layout->addWidget(buttons);

            const auto populateMailboxes = [&]
            {
                mailboxCombo->clear();
                const auto accountIndex = accountCombo->currentIndex();
                if (accountIndex < 0)
                    return;
                const auto accountId = accountCombo->currentData().toString().toStdString();
                if (accountCombo->currentData(Qt::UserRole + 1).toBool())
                {
                    mailboxCombo->addItem(i18n("Account top level"), QString{});
                    mailboxCombo->setItemData(mailboxCombo->count() - 1, true, Qt::UserRole + 1);
                }
                const auto mailboxesResult = mailboxReader.listMailboxTree(accountId);
                const auto* mailboxes =
                    std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(
                        &mailboxesResult);
                if (mailboxes == nullptr)
                    return;
                std::unordered_map<std::string, const javelin::jmap::cache::MailboxTreeItem*> byId;
                byId.reserve(mailboxes->size());
                for (const auto& mailbox : *mailboxes)
                    byId.emplace(mailbox.id, &mailbox);
                std::vector<std::pair<QString, const javelin::jmap::cache::MailboxTreeItem*>>
                    writable;
                for (const auto& mailbox : *mailboxes)
                {
                    if (!mailbox.pendingCreate && mailbox.myRights.mayAddItems)
                        writable.emplace_back(mailboxPath(mailbox, byId), &mailbox);
                }
                std::ranges::sort(
                    writable, [](const auto& left, const auto& right)
                    { return QString::localeAwareCompare(left.first, right.first) < 0; });
                for (const auto& [path, mailbox] : writable)
                {
                    mailboxCombo->addItem(path, QString::fromStdString(mailbox->id));
                    mailboxCombo->setItemData(mailboxCombo->count() - 1,
                                              mailbox->myRights.mayCreateChild, Qt::UserRole + 1);
                }
                if (preferredAccountId.has_value() && accountId == *preferredAccountId &&
                    preferredMailboxId.has_value())
                {
                    const auto wanted = QString::fromStdString(*preferredMailboxId);
                    const auto index = mailboxCombo->findData(wanted);
                    if (index >= 0)
                        mailboxCombo->setCurrentIndex(index);
                }
            };

            const auto updateEnabled = [&]
            {
                const bool recreate = hierarchy != nullptr && hierarchy->isChecked();
                const bool hasAccount = accountCombo->currentIndex() >= 0;
                const bool hasMailbox = mailboxCombo->currentIndex() >= 0 &&
                                        !mailboxCombo->currentData().toString().isEmpty();
                const bool canCreateChildren = mailboxCombo->currentIndex() >= 0 &&
                                               mailboxCombo->currentData(Qt::UserRole + 1).toBool();
                buttons->button(QDialogButtonBox::Open)
                    ->setEnabled(hasAccount && (recreate ? canCreateChildren : hasMailbox));
            };

            QObject::connect(accountCombo, &QComboBox::currentIndexChanged, &dialog,
                             [&](int)
                             {
                                 populateMailboxes();
                                 updateEnabled();
                             });
            QObject::connect(mailboxCombo, &QComboBox::currentIndexChanged, &dialog,
                             [&](int) { updateEnabled(); });
            if (hierarchy != nullptr)
            {
                QObject::connect(hierarchy, &QCheckBox::toggled, &dialog,
                                 [&](bool) { updateEnabled(); });
            }
            QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
            QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

            if (preferredAccountId.has_value())
            {
                const auto index =
                    accountCombo->findData(QString::fromStdString(*preferredAccountId));
                if (index >= 0)
                    accountCombo->setCurrentIndex(index);
            }
            populateMailboxes();
            updateEnabled();

            if (dialog.exec() != QDialog::Accepted)
                return std::optional<MailImportDialogChoice>{};

            const auto mailboxValue = mailboxCombo->currentData().toString();
            return std::optional<MailImportDialogChoice>{MailImportDialogChoice{
                .accountId = accountCombo->currentData().toString().toStdString(),
                .mailboxId = mailboxValue.isEmpty()
                                 ? std::nullopt
                                 : std::optional<std::string>{mailboxValue.toStdString()},
                .recreateHierarchy = hierarchy != nullptr && hierarchy->isChecked(),
            }};
        }
    } // namespace

    MailImportController::MailImportController(javelin::app::MailImportPort& importPort,
                                               javelin::jmap::cache::AccountReader& accountReader,
                                               javelin::jmap::cache::MailboxReader& mailboxReader,
                                               QWidget& dialogParent, QObject* parent)
        : QObject(parent), m_importPort(importPort), m_accountReader(accountReader),
          m_mailboxReader(mailboxReader), m_dialogParent(dialogParent)
    {
    }

    void MailImportController::importMessages(std::optional<std::string> accountId,
                                              std::optional<std::string> mailboxId)
    {
        const auto files = QFileDialog::getOpenFileNames(
            &m_dialogParent, i18n("Import Messages"), QDir::homePath(),
            i18n("Mail files (*.eml *.mbox *.mbx);;All files (*)"));
        if (files.isEmpty())
            return;
        promptAndStart(files, std::move(accountId), std::move(mailboxId), false, false);
    }

    void MailImportController::importFolderTree(std::optional<std::string> accountId,
                                                std::optional<std::string> parentMailboxId)
    {
        const auto directory =
            QFileDialog::getExistingDirectory(&m_dialogParent, i18n("Import Folder Tree"),
                                              QDir::homePath(), QFileDialog::ShowDirsOnly);
        if (directory.isEmpty())
            return;
        promptAndStart({directory}, std::move(accountId), std::move(parentMailboxId), true, true);
    }

    void MailImportController::importDroppedPaths(QStringList paths, std::string accountId,
                                                  std::string mailboxId)
    {
        if (paths.isEmpty())
            return;
        const bool hierarchySource = paths.size() == 1 && QFileInfo{paths.front()}.isDir();
        promptAndStart(std::move(paths), std::move(accountId), std::move(mailboxId),
                       hierarchySource, hierarchySource);
    }

    void MailImportController::promptAndStart(QStringList sourcePaths,
                                              std::optional<std::string> accountId,
                                              std::optional<std::string> mailboxId,
                                              const bool hierarchySource,
                                              const bool hierarchyDefault)
    {
        for (auto& path : sourcePaths)
            path = QFileInfo{path}.absoluteFilePath();
        const auto choice =
            promptMailImport(m_dialogParent, m_accountReader, m_mailboxReader, sourcePaths,
                             accountId, mailboxId, hierarchySource, hierarchyDefault);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&choice))
        {
            Q_EMIT statusMessage(error->message, 10000);
            return;
        }
        const auto& selected = std::get<std::optional<MailImportDialogChoice>>(choice);
        if (!selected.has_value())
            return;

        std::vector<QString> sources;
        sources.reserve(static_cast<std::size_t>(sourcePaths.size()));
        for (const auto& path : sourcePaths)
            sources.push_back(path);
        auto task = m_importPort.startImport({
            .accountId = selected->accountId,
            .mailboxId = selected->mailboxId,
            .sourcePaths = std::move(sources),
            .recreateHierarchy = selected->recreateHierarchy,
        });
        QCoro::connect(
            std::move(task), this,
            [this](javelin::app::MailImportStartResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    Q_EMIT operationFailed(*error);
                    return;
                }
                Q_EMIT statusMessage(
                    i18n("Mail import started. Progress is available in Task Center."), 5000);
            });
    }
} // namespace javelin::gui::shell
