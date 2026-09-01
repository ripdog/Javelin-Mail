#include "gui/shell/MessageCommandController.h"

#include "gui/messages/MessageActionSelection.h"
#include "gui/messages/MessageConfirmationPresentation.h"
#include "gui/messages/MessageListModel.h"
#include "gui/shell/MessageTransferDestinationMenu.h"
#include "jmap/cache/MailboxReadRepository.h"

#include <QCoroTask>

#include <KLocalizedString>

#include <QAction>
#include <QCursor>
#include <QItemSelectionModel>
#include <QListView>
#include <QLoggingCategory>
#include <QMenu>
#include <QMessageBox>
#include <QPersistentModelIndex>
#include <QPushButton>

#include <ranges>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace javelin::gui::shell
{
    Q_LOGGING_CATEGORY(logMessageCommands, "user.operations")

    namespace
    {
        [[nodiscard]] std::optional<javelin::jmap::cache::MailboxTreeItem>
        findMailboxByRole(javelin::jmap::cache::MailboxReader& mailboxReader,
                          const std::string_view accountId, const std::string_view role)
        {
            const auto result = mailboxReader.listMailboxTree(accountId);
            const auto* mailboxes =
                std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&result);
            if (mailboxes == nullptr)
            {
                return std::nullopt;
            }

            const auto mailbox =
                std::ranges::find(*mailboxes, std::optional<std::string>{std::string{role}},
                                  &javelin::jmap::cache::MailboxTreeItem::role);
            return mailbox == mailboxes->end()
                       ? std::optional<javelin::jmap::cache::MailboxTreeItem>{std::nullopt}
                       : std::optional<javelin::jmap::cache::MailboxTreeItem>{*mailbox};
        }
    } // namespace

    MessageCommandController::MessageCommandController(
        javelin::app::MailCommandPort& mailCommandPort,
        javelin::jmap::cache::AccountReader& accountReader,
        javelin::jmap::cache::MailboxReader& mailboxReader,
        MessageTransferAccountDisplayName accountDisplayName, QListView& messageView,
        QWidget* dialogParent, QObject* parent)
        : QObject(parent), m_mailCommandPort(mailCommandPort), m_accountReader(accountReader),
          m_mailboxReader(mailboxReader), m_accountDisplayName(std::move(accountDisplayName)),
          m_messageView(messageView), m_dialogParent(dialogParent)
    {
    }

    javelin::app::MessageSelection
    MessageCommandController::selectedActionItems(const bool excludeUnread) const
    {
        const auto* selectionModel = m_messageView.selectionModel();
        const auto selectedRows =
            selectionModel == nullptr ? QModelIndexList{} : selectionModel->selectedRows();
        return javelin::gui::messages::messageSelectionForAction(
            selectedRows, m_messageView.currentIndex(), excludeUnread);
    }

    void MessageCommandController::archiveSelection(std::optional<std::string> accountId,
                                                    std::optional<std::string> sourceMailboxId,
                                                    const bool searchTab)
    {
        if (!accountId.has_value() || (!sourceMailboxId.has_value() && !searchTab))
        {
            Q_EMIT statusMessage(i18n("Select a message to archive."), 3000);
            return;
        }

        auto selection = selectedActionItems();
        if (selection.empty())
        {
            Q_EMIT statusMessage(i18n("Select a message to archive."), 3000);
            return;
        }
        queueArchive(std::move(*accountId), std::move(sourceMailboxId), std::move(selection));
    }

    void MessageCommandController::deleteSelection(std::optional<std::string> accountId,
                                                   std::optional<std::string> sourceMailboxId)
    {
        if (!accountId.has_value())
        {
            Q_EMIT statusMessage(i18n("Select a message to delete."), 3000);
            return;
        }

        auto selection = selectedActionItems();
        if (selection.empty())
        {
            Q_EMIT statusMessage(i18n("Select a message to delete."), 3000);
            return;
        }

        // The open tab can expose a Thread child resident outside that mailbox. Never infer
        // permanent deletion from the tab being Trash; that remains a separate explicit action.
        queueDelete(std::move(*accountId), std::move(sourceMailboxId), std::move(selection));
    }

    void
    MessageCommandController::permanentlyDeleteSelection(std::optional<std::string> accountId,
                                                         std::optional<std::string> sourceMailboxId)
    {
        if (!accountId.has_value())
        {
            Q_EMIT statusMessage(i18n("Select a message to delete."), 3000);
            return;
        }

        auto selection = selectedActionItems();
        if (selection.empty())
        {
            Q_EMIT statusMessage(i18n("Select a message to delete."), 3000);
            return;
        }
        if (!confirmPermanentDelete(selection))
        {
            return;
        }
        queueDestroy(std::move(*accountId), std::move(sourceMailboxId), std::move(selection));
    }

    void MessageCommandController::showTransferMenu(const MessageTransferOperation operation,
                                                    std::optional<std::string> accountId,
                                                    std::optional<std::string> sourceMailboxId,
                                                    const bool searchTab)
    {
        const bool move = operation == MessageTransferOperation::Move;
        if (!accountId.has_value() || (!sourceMailboxId.has_value() && !searchTab))
        {
            Q_EMIT statusMessage(
                move ? i18n("Select a message to move.") : i18n("Select a message to copy."), 3000);
            return;
        }

        auto selection = selectedActionItems();
        if (selection.empty())
        {
            Q_EMIT statusMessage(
                move ? i18n("Select a message to move.") : i18n("Select a message to copy."), 3000);
            return;
        }

        QMenu menu{m_dialogParent.data()};
        menu.setTitle(move ? i18n("Move to") : i18n("Copy to"));
        const bool hasDestinations = populateDestinationMenus(
            move ? &menu : nullptr, move ? nullptr : &menu, std::move(*accountId),
            std::move(sourceMailboxId), std::move(selection));
        if (!hasDestinations)
        {
            Q_EMIT statusMessage(i18n("No destination mailboxes available."), 3000);
            return;
        }
        menu.exec(QCursor::pos());
    }

    bool MessageCommandController::populateDestinationMenus(
        QMenu* moveMenu, QMenu* copyMenu, std::string accountId,
        std::optional<std::string> sourceMailboxId, javelin::app::MessageSelection selection)
    {
        std::unordered_map<std::string, std::vector<javelin::jmap::cache::MailboxTreeItem>>
            mailboxesByAccount;
        const auto currentMailboxes = m_mailboxReader.listMailboxTree(accountId);
        const auto* current =
            std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&currentMailboxes);
        if (current == nullptr)
            return false;
        mailboxesByAccount.emplace(accountId, *current);

        std::vector<javelin::jmap::cache::CachedAccount> accounts;
        const auto accountsResult = m_accountReader.listAll();
        if (const auto* loaded =
                std::get_if<std::vector<javelin::jmap::cache::CachedAccount>>(&accountsResult))
        {
            accounts = *loaded;
            for (const auto& account : accounts)
            {
                if (account.accountId == accountId || !account.hasMailCapability ||
                    account.isReadOnly)
                    continue;
                const auto mailboxResult = m_mailboxReader.listMailboxTree(account.accountId);
                if (const auto* mailboxes =
                        std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(
                            &mailboxResult))
                    mailboxesByAccount.emplace(account.accountId, *mailboxes);
            }
        }

        const auto presentation = buildMessageTransferDestinationPresentation(
            accountId, accounts, mailboxesByAccount, m_accountDisplayName);
        const bool hasDestinations =
            !presentation.currentAccountRows.empty() || !presentation.otherAccounts.empty();
        if (!hasDestinations)
            return false;

        const auto populate = [this, &presentation, &accountId, &sourceMailboxId,
                               &selection](QMenu* menu, const MessageTransferOperation operation,
                                           const QString& successMessage)
        {
            if (menu == nullptr)
                return;
            static_cast<void>(populateMessageTransferDestinationMenu(
                *menu, presentation,
                [this, sourceAccountId = accountId, sourceMailboxId, selection, operation,
                 successMessage](const MessageTransferDestinationRow& destination)
                {
                    queueTransfer(sourceAccountId, sourceMailboxId, destination.accountId,
                                  destination.mailboxId, selection, operation, successMessage);
                }));
        };

        // The open mailbox scopes collapsed-thread resolution; it does not describe the
        // residency of every visible or selected Email. Keep every writable mailbox available so
        // mixed selections can copy or move the members that are not already in that destination.
        populate(moveMenu, MessageTransferOperation::Move, i18n("Queued move."));
        populate(copyMenu, MessageTransferOperation::Copy, i18n("Queued copy."));
        return true;
    }

    void MessageCommandController::queueTransfer(std::string sourceAccountId,
                                                 std::optional<std::string> sourceMailboxId,
                                                 std::string destinationAccountId,
                                                 std::string destinationMailboxId,
                                                 javelin::app::MessageSelection selection,
                                                 const MessageTransferOperation operation,
                                                 QString successMessage)
    {
        const bool move = operation == MessageTransferOperation::Move;
        qCInfo(logMessageCommands).noquote()
            << (move ? "move requested" : "copy requested") << selection.size()
            << "selection item(s) from" << QString::fromStdString(sourceAccountId) << "to"
            << QString::fromStdString(destinationAccountId) << '/'
            << QString::fromStdString(destinationMailboxId);

        if (sourceAccountId != destinationAccountId)
        {
            auto task = m_mailCommandPort.transferAcrossAccounts({
                .sourceAccountId = sourceAccountId,
                .sourceMailboxId = sourceMailboxId,
                .destinationAccountId = destinationAccountId,
                .destinationMailboxId = destinationMailboxId,
                .operation = move ? javelin::app::MailTransferOperation::Move
                                  : javelin::app::MailTransferOperation::Copy,
                .selection = std::move(selection),
            });
            QCoro::connect(
                std::move(task), this,
                [this, sourceAccountId = std::move(sourceAccountId),
                 destinationAccountId = std::move(destinationAccountId),
                 move](javelin::app::MailTransferExecutionResult result)
                {
                    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                    {
                        Q_EMIT operationFailed(*error);
                        return;
                    }
                    const auto& summary =
                        std::get<javelin::app::MailTransferExecutionSummary>(result);
                    if (summary.status == javelin::app::MailTransferStatus::Complete)
                    {
                        Q_EMIT statusMessage(
                            move ? i18np("Moved one message.", "Moved %1 messages.",
                                         summary.completeItemCount)
                                 : i18np("Copied one message.", "Copied %1 messages.",
                                         summary.completeItemCount),
                            5000);
                        return;
                    }

                    if (summary.status == javelin::app::MailTransferStatus::Preparing ||
                        summary.status == javelin::app::MailTransferStatus::Running)
                    {
                        Q_EMIT statusMessage(i18n("The transfer is continuing in the background."),
                                             5000);
                        return;
                    }
                    if (summary.status == javelin::app::MailTransferStatus::WaitingForNetwork)
                    {
                        Q_EMIT statusMessage(
                            i18n("The transfer is waiting for network access and will resume "
                                 "automatically."),
                            10000);
                        return;
                    }
                    if (summary.status == javelin::app::MailTransferStatus::WaitingForAuth)
                    {
                        Q_EMIT statusMessage(
                            i18n("The transfer is waiting for sign-in and will resume "
                                 "automatically."),
                            10000);
                        return;
                    }
                    if (summary.status == javelin::app::MailTransferStatus::WaitingForSpace)
                    {
                        Q_EMIT statusMessage(i18n("The transfer is waiting for storage space."),
                                             10000);
                        return;
                    }

                    QString message;
                    javelin::jmap::OperationErrorCode code =
                        javelin::jmap::OperationErrorCode::Conflict;
                    switch (summary.status)
                    {
                    case javelin::app::MailTransferStatus::BlockedUnknown:
                        message = i18np(
                            "The transfer outcome could not be confirmed for one message. Javelin "
                            "will not retry it automatically.",
                            "The transfer outcome could not be confirmed for %1 messages. Javelin "
                            "will not retry them automatically.",
                            summary.unknownItemCount);
                        break;
                    case javelin::app::MailTransferStatus::Partial:
                        message = i18n(
                            "The transfer completed partially: %1 complete, %2 retained at the "
                            "source, and %3 failed.",
                            summary.completeItemCount, summary.partialItemCount,
                            summary.failedItemCount);
                        break;
                    case javelin::app::MailTransferStatus::Failed:
                        code = javelin::jmap::OperationErrorCode::ServerFailure;
                        message =
                            i18np("The transfer failed for one message.",
                                  "The transfer failed for %1 messages.", summary.failedItemCount);
                        break;
                    case javelin::app::MailTransferStatus::Cancelled:
                        message = i18n("The transfer was cancelled.");
                        break;
                    case javelin::app::MailTransferStatus::Preparing:
                    case javelin::app::MailTransferStatus::Running:
                    case javelin::app::MailTransferStatus::WaitingForNetwork:
                    case javelin::app::MailTransferStatus::WaitingForAuth:
                    case javelin::app::MailTransferStatus::WaitingForSpace:
                        Q_UNREACHABLE();
                    case javelin::app::MailTransferStatus::Complete:
                        Q_UNREACHABLE();
                    }
                    Q_EMIT operationFailed({.code = code, .message = std::move(message)});
                });
            return;
        }

        auto task = m_mailCommandPort.queueMailboxSelectionMutation(
            javelin::app::MailboxSelectionMutationIntent{
                .accountId = sourceAccountId,
                .selection = std::move(selection),
                .operation = move ? javelin::app::MailboxSelectionOperation::Move
                                  : javelin::app::MailboxSelectionOperation::Copy,
                .sourceMailboxId = std::move(sourceMailboxId),
                .destinationMailboxId = std::move(destinationMailboxId),
            });
        QCoro::connect(
            std::move(task), this,
            [this, accountId = std::move(sourceAccountId), move,
             successMessage = std::move(successMessage)](
                javelin::app::QueuedMailboxSelectionMutationResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    Q_EMIT operationFailed(*error);
                    return;
                }

                const auto& summary =
                    std::get<javelin::app::QueuedMailboxSelectionMutation>(result);
                if (summary.queuedEmailCount == 0)
                {
                    Q_EMIT statusMessage(i18n("The selected messages are already there."), 5000);
                    return;
                }

                if (summary.skippedEmailCount > 0)
                {
                    Q_EMIT statusMessage(
                        move ? i18n("Queued move for %1 messages; skipped %2 already there.",
                                    summary.queuedEmailCount, summary.skippedEmailCount)
                             : i18n("Queued copy for %1 messages; skipped %2 already there.",
                                    summary.queuedEmailCount, summary.skippedEmailCount),
                        5000);
                }
                else if (summary.queuedEmailCount > 1)
                {
                    auto message = successMessage;
                    if (message.endsWith(QLatin1Char('.')))
                        message.chop(1);
                    Q_EMIT statusMessage(
                        i18n("%1 for %2 messages.", message, summary.queuedEmailCount), 5000);
                }
                else
                {
                    Q_EMIT statusMessage(successMessage, 5000);
                }
                submitQueuedMutations(accountId,
                                      summary.queuedMutations.front().patch.operationGroupId);
            });
    }

    void MessageCommandController::markEmailRead(std::string accountId, std::string emailId)
    {
        qCInfo(logMessageCommands) << "mark read requested";
        auto task = m_mailCommandPort.queueMarkEmailRead(accountId, emailId);
        QCoro::connect(
            std::move(task), this,
            [this, accountId = std::move(accountId)](
                javelin::app::QueuedMessageSelectionMutationResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    Q_EMIT operationFailed(*error);
                    return;
                }

                const auto& summary =
                    std::get<javelin::app::QueuedMessageSelectionMutation>(result);
                if (summary.queuedEmailCount == 0)
                    return;
                submitQueuedMutations(accountId,
                                      summary.queuedMutations.front().patch.operationGroupId);
            });
    }

    void MessageCommandController::toggleFlagged(std::optional<std::string> accountId,
                                                 const QModelIndex& index)
    {
        if (!accountId.has_value() || !index.isValid())
        {
            return;
        }

        const auto emailId =
            index.data(javelin::gui::messages::MessageListModel::EmailIdRole).toString();
        if (emailId.isEmpty())
        {
            return;
        }

        const bool flagged =
            index.data(javelin::gui::messages::MessageListModel::IsFlaggedRole).toBool();
        qCInfo(logMessageCommands) << (flagged ? "remove star requested" : "add star requested");
        javelin::app::MessageSelection selection;
        selection.emplace_back(javelin::app::SelectedEmail{.emailId = emailId.toStdString()});
        auto task = m_mailCommandPort.queueSetMessagesFlagged(*accountId, std::nullopt,
                                                              std::move(selection), !flagged);
        QCoro::connect(
            std::move(task), this,
            [this, accountId = std::move(*accountId), index = QPersistentModelIndex{index},
             flagged](javelin::app::QueuedMessageSelectionMutationResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    Q_EMIT operationFailed(*error);
                    return;
                }

                const auto& summary =
                    std::get<javelin::app::QueuedMessageSelectionMutation>(result);
                if (summary.queuedEmailCount == 0)
                    return;
                if (index.isValid())
                    m_messageView.setCurrentIndex(index);
                Q_EMIT statusMessage(flagged ? i18n("Removed star.") : i18n("Added star."), 5000);
                submitQueuedMutations(accountId,
                                      summary.queuedMutations.front().patch.operationGroupId);
            });
    }

    void MessageCommandController::setSelectionFlagged(std::optional<std::string> accountId,
                                                       std::optional<std::string> sourceMailboxId,
                                                       const bool flagged)
    {
        if (!accountId.has_value())
        {
            Q_EMIT statusMessage(i18n("Select a message to change its star."), 3000);
            return;
        }
        auto selection = selectedActionItems();
        if (selection.empty())
            return;

        qCInfo(logMessageCommands) << (flagged ? "add star requested" : "remove star requested")
                                   << selection.size() << "selection item(s)";
        auto task = m_mailCommandPort.queueSetMessagesFlagged(
            *accountId, std::move(sourceMailboxId), std::move(selection), flagged);
        QCoro::connect(
            std::move(task), this,
            [this, accountId = std::move(*accountId),
             flagged](javelin::app::QueuedMessageSelectionMutationResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    Q_EMIT operationFailed(*error);
                    return;
                }

                const auto& summary =
                    std::get<javelin::app::QueuedMessageSelectionMutation>(result);
                if (summary.queuedEmailCount == 0 || summary.queuedMutations.empty())
                    return;
                Q_EMIT statusMessage(
                    flagged ? i18np("Added a star to %1 message.", "Added a star to %1 messages.",
                                    summary.queuedEmailCount)
                            : i18np("Removed the star from %1 message.",
                                    "Removed the star from %1 messages.", summary.queuedEmailCount),
                    5000);
                submitQueuedMutations(accountId,
                                      summary.queuedMutations.front().patch.operationGroupId);
            });
    }

    void MessageCommandController::markSelectionUnread(std::optional<std::string> accountId,
                                                       std::optional<std::string> sourceMailboxId)
    {
        if (!accountId.has_value() || m_messageView.selectionModel() == nullptr)
        {
            Q_EMIT statusMessage(i18n("Select a message to mark unread."), 3000);
            return;
        }

        auto selection = selectedActionItems(true);
        if (selection.empty())
        {
            return;
        }

        qCInfo(logMessageCommands)
            << "mark unread requested" << selection.size() << "selection item(s)";
        auto task = m_mailCommandPort.queueMarkMessagesUnread(
            *accountId, std::move(sourceMailboxId), std::move(selection));
        QCoro::connect(
            std::move(task), this,
            [this, accountId = std::move(*accountId)](
                javelin::app::QueuedMessageSelectionMutationResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    Q_EMIT operationFailed(*error);
                    return;
                }

                const auto& summary =
                    std::get<javelin::app::QueuedMessageSelectionMutation>(result);
                const auto markedCount = summary.queuedEmailCount;
                if (markedCount == 0)
                    return;
                Q_EMIT statusMessage(markedCount == 1
                                         ? i18n("Marked unread.")
                                         : i18np("Marked %1 message unread.",
                                                 "Marked %1 messages unread.", markedCount),
                                     5000);
                submitQueuedMutations(accountId,
                                      summary.queuedMutations.front().patch.operationGroupId);
            });
    }

    void MessageCommandController::setSelectionJunk(std::optional<std::string> accountId,
                                                    std::optional<std::string> sourceMailboxId,
                                                    const bool junk)
    {
        if (!accountId.has_value())
        {
            Q_EMIT statusMessage(i18n("Select a message to classify."), 3000);
            return;
        }

        auto selection = selectedActionItems();
        if (selection.empty())
        {
            Q_EMIT statusMessage(i18n("Select a message to classify."), 3000);
            return;
        }
        queueJunk(std::move(*accountId), std::move(sourceMailboxId), std::move(selection), junk);
    }

    void MessageCommandController::setEmailJunk(std::string accountId,
                                                std::optional<std::string> sourceMailboxId,
                                                std::string emailId, const bool junk)
    {
        if (accountId.empty() || emailId.empty())
        {
            return;
        }
        javelin::app::MessageSelection selection;
        selection.emplace_back(javelin::app::SelectedEmail{.emailId = std::move(emailId)});
        queueJunk(std::move(accountId), std::move(sourceMailboxId), std::move(selection), junk);
    }

    void MessageCommandController::setSelectionTag(std::optional<std::string> accountId,
                                                   std::optional<std::string> sourceMailboxId,
                                                   std::string keyword, const bool enabled)
    {
        if (!accountId.has_value())
        {
            Q_EMIT statusMessage(i18n("Select a message to change tags."), 3000);
            return;
        }
        auto selection = selectedActionItems();
        if (selection.empty())
            return;

        auto task = m_mailCommandPort.queueSetMessagesTag(*accountId, std::move(sourceMailboxId),
                                                          std::move(selection), std::move(keyword),
                                                          enabled);
        QCoro::connect(
            std::move(task), this,
            [this, accountId = std::move(*accountId),
             enabled](javelin::app::QueuedMessageSelectionMutationResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    Q_EMIT operationFailed(*error);
                    return;
                }
                const auto& summary =
                    std::get<javelin::app::QueuedMessageSelectionMutation>(result);
                if (summary.queuedEmailCount == 0 || summary.queuedMutations.empty())
                    return;
                Q_EMIT statusMessage(
                    enabled ? i18np("Added tag to %1 message.", "Added tag to %1 messages.",
                                    summary.queuedEmailCount)
                            : i18np("Removed tag from %1 message.", "Removed tag from %1 messages.",
                                    summary.queuedEmailCount),
                    5000);
                submitQueuedMutations(accountId,
                                      summary.queuedMutations.front().patch.operationGroupId);
            });
    }

    void MessageCommandController::queueJunk(std::string accountId,
                                             std::optional<std::string> sourceMailboxId,
                                             javelin::app::MessageSelection selection,
                                             const bool junk)
    {
        qCInfo(logMessageCommands) << (junk ? "junk requested" : "not junk requested")
                                   << selection.size() << "selection item(s)";
        auto task = m_mailCommandPort.queueMailboxSelectionMutation(
            javelin::app::MailboxSelectionMutationIntent{
                .accountId = accountId,
                .selection = std::move(selection),
                .operation = junk ? javelin::app::MailboxSelectionOperation::Junk
                                  : javelin::app::MailboxSelectionOperation::NotJunk,
                .sourceMailboxId = std::move(sourceMailboxId),
                .destinationMailboxId = std::nullopt,
            });
        QCoro::connect(
            std::move(task), this,
            [this, accountId = std::move(accountId),
             junk](javelin::app::QueuedMailboxSelectionMutationResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    Q_EMIT operationFailed(*error);
                    return;
                }

                const auto& summary =
                    std::get<javelin::app::QueuedMailboxSelectionMutation>(result);
                if (summary.queuedEmailCount == 0 || summary.queuedMutations.empty())
                {
                    Q_EMIT statusMessage(junk ? i18n("Already marked as junk.")
                                              : i18n("Already marked as not junk."),
                                         5000);
                    return;
                }

                Q_EMIT statusMessage(
                    summary.queuedEmailCount == 1
                        ? (junk ? i18n("Marked as junk.") : i18n("Marked as not junk."))
                        : (junk ? i18np("Marked %1 message as junk.", "Marked %1 messages as junk.",
                                        summary.queuedEmailCount)
                                : i18np("Marked %1 message as not junk.",
                                        "Marked %1 messages as not junk.",
                                        summary.queuedEmailCount)),
                    5000);
                submitQueuedMutations(accountId,
                                      summary.queuedMutations.front().patch.operationGroupId);
            });
    }

    void MessageCommandController::queueArchive(std::string accountId,
                                                std::optional<std::string> sourceMailboxId,
                                                javelin::app::MessageSelection selection)
    {
        const bool searchArchive = !sourceMailboxId.has_value();
        auto task = m_mailCommandPort.queueMailboxSelectionMutation(
            javelin::app::MailboxSelectionMutationIntent{
                .accountId = accountId,
                .selection = std::move(selection),
                .operation = javelin::app::MailboxSelectionOperation::Archive,
                .sourceMailboxId = std::move(sourceMailboxId),
                .destinationMailboxId = std::nullopt,
            });
        QCoro::connect(
            std::move(task), this,
            [this, accountId = std::move(accountId),
             searchArchive](javelin::app::QueuedMailboxSelectionMutationResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    Q_EMIT operationFailed(*error);
                    return;
                }

                const auto& summary =
                    std::get<javelin::app::QueuedMailboxSelectionMutation>(result);
                if (summary.queuedEmailCount == 0)
                {
                    Q_EMIT statusMessage(searchArchive
                                             ? i18n("The selected messages are not in Inbox.")
                                             : i18n("The selected messages are already archived."),
                                         5000);
                    return;
                }

                if (summary.skippedEmailCount > 0)
                {
                    Q_EMIT statusMessage(
                        i18n("Queued archive for %1 messages; skipped %2 not in Inbox.",
                             summary.queuedEmailCount, summary.skippedEmailCount),
                        5000);
                }
                else
                {
                    Q_EMIT statusMessage(summary.queuedEmailCount == 1
                                             ? i18n("Queued archive.")
                                             : i18np("Queued archive for %1 message.",
                                                     "Queued archive for %1 messages.",
                                                     summary.queuedEmailCount),
                                         5000);
                }
                submitQueuedMutations(accountId,
                                      summary.queuedMutations.front().patch.operationGroupId);
            });
    }

    void MessageCommandController::queueDelete(std::string accountId,
                                               std::optional<std::string> sourceMailboxId,
                                               javelin::app::MessageSelection selection)
    {
        const auto trashMailbox = findMailboxByRole(m_mailboxReader, accountId, "trash");
        if (!trashMailbox.has_value())
        {
            Q_EMIT statusMessage(i18n("No Trash mailbox is available."), 5000);
            return;
        }
        const auto destinationAccountId = accountId;
        queueTransfer(std::move(accountId), std::move(sourceMailboxId), destinationAccountId,
                      trashMailbox->id, std::move(selection), MessageTransferOperation::Move,
                      i18n("Queued delete."));
    }

    void MessageCommandController::queueDestroy(std::string accountId,
                                                std::optional<std::string> sourceMailboxId,
                                                javelin::app::MessageSelection selection)
    {
        qCInfo(logMessageCommands)
            << "permanently delete requested" << selection.size() << "selection item(s)";
        auto task = m_mailCommandPort.queueDestroyMessages(accountId, std::move(sourceMailboxId),
                                                           std::move(selection));
        QCoro::connect(
            std::move(task), this,
            [this, accountId = std::move(accountId)](
                javelin::app::QueuedMessageSelectionMutationResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    Q_EMIT operationFailed(*error);
                    return;
                }

                const auto& summary =
                    std::get<javelin::app::QueuedMessageSelectionMutation>(result);
                const auto selectedCount = summary.queuedEmailCount;
                if (selectedCount == 0 || summary.queuedMutations.empty())
                    return;
                Q_EMIT statusMessage(selectedCount == 1
                                         ? i18n("Queued permanent deletion.")
                                         : i18np("Queued permanent deletion for %1 message.",
                                                 "Queued permanent deletion for %1 messages.",
                                                 selectedCount),
                                     5000);
                submitQueuedMutations(accountId,
                                      summary.queuedMutations.front().patch.operationGroupId);
            });
    }

    void
    MessageCommandController::submitQueuedMutations(std::string accountId,
                                                    std::optional<std::string> operationGroupId)
    {
        auto task =
            m_mailCommandPort.submitPendingEmailMutations(accountId, std::move(operationGroupId));
        QCoro::connect(std::move(task), this,
                       [this](javelin::jmap::SubmittedEmailMutationsResult submitResult)
                       {
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&submitResult))
                           {
                               Q_EMIT operationFailed(*error);
                               return;
                           }

                           const auto& summary =
                               std::get<javelin::jmap::SubmittedEmailMutations>(submitResult);
                           Q_EMIT emailMutationsSubmitted({
                               .accountId = summary.accountId,
                               .updatedEmailCount = summary.updatedEmailCount,
                               .failedEmailCount = summary.failedEmailCount,
                           });
                       });
    }

    bool MessageCommandController::confirmPermanentDelete(
        const javelin::app::MessageSelection& selection) const
    {
        const auto* selectionModel = m_messageView.selectionModel();
        const auto selectedRows =
            selectionModel == nullptr ? QModelIndexList{} : selectionModel->selectedRows();
        const auto presentation = javelin::gui::messages::permanentDeleteConfirmation(
            selection, selectedRows, m_messageView.currentIndex());

        QMessageBox confirmation{QMessageBox::Warning, i18n("Delete Permanently"),
                                 presentation.prompt, QMessageBox::NoButton, m_dialogParent.data()};
        confirmation.setInformativeText(presentation.details);
        auto* deleteButton =
            confirmation.addButton(i18n("Delete Permanently"), QMessageBox::DestructiveRole);
        confirmation.addButton(QMessageBox::Cancel);
        confirmation.setDefaultButton(QMessageBox::Cancel);
        confirmation.exec();
        return confirmation.clickedButton() == deleteButton;
    }

} // namespace javelin::gui::shell
