#include "gui/shell/MessageCommandController.h"

#include "app/MailApplicationService.h"
#include "gui/mailboxes/MailboxSort.h"
#include "gui/messages/MessageActionSelection.h"
#include "gui/messages/MessageListModel.h"
#include "jmap/cache/QueryService.h"

#include <QAction>
#include <QCursor>
#include <QItemSelectionModel>
#include <QListView>
#include <QLoggingCategory>
#include <QMenu>
#include <QMessageBox>

#include <ranges>
#include <utility>
#include <variant>
#include <vector>

namespace javelin::gui::shell
{
    Q_LOGGING_CATEGORY(logMessageCommands, "user.operations")

    namespace
    {
        [[nodiscard]] std::optional<javelin::jmap::cache::MailboxTreeItem>
        findMailboxByRole(javelin::jmap::cache::QueryService& queryService,
                          const std::string_view accountId, const std::string_view role)
        {
            const auto result = queryService.listMailboxTree(accountId);
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
        javelin::app::MailApplicationService& mailService,
        javelin::jmap::cache::QueryService& queryService, QListView& messageView,
        QWidget* dialogParent, QObject* parent)
        : QObject(parent), m_mailService(mailService), m_queryService(queryService),
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
            Q_EMIT statusMessage(QStringLiteral("Select a message to archive."), 3000);
            return;
        }

        auto selection = selectedActionItems();
        if (selection.empty())
        {
            Q_EMIT statusMessage(QStringLiteral("Select a message to archive."), 3000);
            return;
        }
        queueArchive(std::move(*accountId), std::move(sourceMailboxId), std::move(selection));
    }

    void MessageCommandController::deleteSelection(std::optional<std::string> accountId,
                                                   std::optional<std::string> sourceMailboxId)
    {
        if (!accountId.has_value() || !sourceMailboxId.has_value())
        {
            Q_EMIT statusMessage(QStringLiteral("Select a message to delete."), 3000);
            return;
        }

        auto selection = selectedActionItems();
        if (selection.empty())
        {
            Q_EMIT statusMessage(QStringLiteral("Select a message to delete."), 3000);
            return;
        }

        const auto trashMailbox = findMailboxByRole(m_queryService, *accountId, "trash");
        if (trashMailbox.has_value() && trashMailbox->id == *sourceMailboxId)
        {
            if (confirmPermanentDelete(selection.size()))
            {
                queueDestroy(std::move(*accountId), std::move(sourceMailboxId),
                             std::move(selection));
            }
            return;
        }

        queueDelete(std::move(*accountId), std::move(*sourceMailboxId), std::move(selection));
    }

    void
    MessageCommandController::permanentlyDeleteSelection(std::optional<std::string> accountId,
                                                         std::optional<std::string> sourceMailboxId)
    {
        if (!accountId.has_value())
        {
            Q_EMIT statusMessage(QStringLiteral("Select a message to delete."), 3000);
            return;
        }

        auto selection = selectedActionItems();
        if (selection.empty())
        {
            Q_EMIT statusMessage(QStringLiteral("Select a message to delete."), 3000);
            return;
        }
        if (!confirmPermanentDelete(selection.size()))
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
        const auto verb = move ? QStringLiteral("move") : QStringLiteral("copy");
        if (!accountId.has_value() || (!sourceMailboxId.has_value() && !searchTab))
        {
            Q_EMIT statusMessage(QStringLiteral("Select a message to %1.").arg(verb), 3000);
            return;
        }

        auto selection = selectedActionItems();
        if (selection.empty())
        {
            Q_EMIT statusMessage(QStringLiteral("Select a message to %1.").arg(verb), 3000);
            return;
        }

        QMenu menu{m_dialogParent.data()};
        menu.setTitle(move ? QStringLiteral("Move to") : QStringLiteral("Copy to"));
        populateDestinationMenus(move ? &menu : nullptr, move ? nullptr : &menu,
                                 std::move(*accountId), std::move(sourceMailboxId),
                                 std::move(selection));
        if (menu.actions().empty())
        {
            Q_EMIT statusMessage(QStringLiteral("No destination mailboxes available."), 3000);
            return;
        }
        menu.exec(QCursor::pos());
    }

    void MessageCommandController::populateDestinationMenus(
        QMenu* moveMenu, QMenu* copyMenu, std::string accountId,
        std::optional<std::string> sourceMailboxId, javelin::app::MessageSelection selection)
    {
        const auto mailboxesResult = m_queryService.listMailboxTree(accountId);
        const auto* mailboxes =
            std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&mailboxesResult);
        if (mailboxes == nullptr)
        {
            return;
        }

        const auto addDestination = [this, &accountId, &sourceMailboxId, &selection](
                                        QMenu* menu, const MessageTransferOperation operation,
                                        const QString& successMessage, const auto& mailbox)
        {
            if (menu == nullptr)
            {
                return;
            }
            auto* action = menu->addAction(QString::fromStdString(mailbox.name));
            connect(action, &QAction::triggered, this,
                    [this, accountId, sourceMailboxId, destinationMailboxId = mailbox.id, selection,
                     operation, successMessage]
                    {
                        queueTransfer(accountId, sourceMailboxId, destinationMailboxId, selection,
                                      operation, successMessage);
                    });
        };

        for (const auto* mailbox : javelin::gui::mailboxes::mailboxesInDisplayOrder(*mailboxes))
        {
            if ((sourceMailboxId.has_value() && mailbox->id == *sourceMailboxId) ||
                !mailbox->myRights.mayAddItems)
            {
                continue;
            }
            addDestination(moveMenu, MessageTransferOperation::Move, QStringLiteral("Queued move."),
                           *mailbox);
            addDestination(copyMenu, MessageTransferOperation::Copy, QStringLiteral("Queued copy."),
                           *mailbox);
        }
    }

    void MessageCommandController::queueTransfer(std::string accountId,
                                                 std::optional<std::string> sourceMailboxId,
                                                 std::string destinationMailboxId,
                                                 javelin::app::MessageSelection selection,
                                                 const MessageTransferOperation operation,
                                                 QString successMessage)
    {
        const bool move = operation == MessageTransferOperation::Move;
        qCInfo(logMessageCommands).noquote()
            << (move ? "move requested" : "copy requested") << selection.size()
            << "selection item(s) to" << QString::fromStdString(destinationMailboxId);
        const auto result = m_mailService.queueMailboxSelectionMutation(
            javelin::app::MailboxSelectionMutationIntent{
                .accountId = accountId,
                .selection = std::move(selection),
                .operation = move ? javelin::app::MailboxSelectionOperation::Move
                                  : javelin::app::MailboxSelectionOperation::Copy,
                .sourceMailboxId = std::move(sourceMailboxId),
                .destinationMailboxId = std::move(destinationMailboxId),
            });
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            Q_EMIT operationFailed(*error);
            return;
        }

        const auto& summary = std::get<javelin::app::QueuedMailboxSelectionMutation>(result);
        if (summary.queuedEmailCount == 0)
        {
            Q_EMIT statusMessage(QStringLiteral("The selected messages are already there."), 5000);
            return;
        }

        Q_EMIT mailboxMembershipChanged(QString::fromStdString(accountId));
        if (summary.skippedEmailCount > 0)
        {
            Q_EMIT statusMessage(
                QStringLiteral("Queued %1 for %2 messages; skipped %3 already there.")
                    .arg(move ? QStringLiteral("move") : QStringLiteral("copy"))
                    .arg(summary.queuedEmailCount)
                    .arg(summary.skippedEmailCount),
                5000);
        }
        else if (summary.queuedEmailCount > 1)
        {
            if (successMessage.endsWith(QLatin1Char('.')))
            {
                successMessage.chop(1);
            }
            Q_EMIT statusMessage(QStringLiteral("%1 for %2 messages.")
                                     .arg(successMessage)
                                     .arg(summary.queuedEmailCount),
                                 5000);
        }
        else
        {
            Q_EMIT statusMessage(std::move(successMessage), 5000);
        }
        Q_EMIT submitRequested(QString::fromStdString(accountId));
    }

    void MessageCommandController::markEmailRead(std::string accountId, std::string emailId)
    {
        qCInfo(logMessageCommands) << "mark read requested";
        const auto result = m_mailService.queueMarkEmailRead(accountId, std::move(emailId));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            Q_EMIT operationFailed(*error);
            return;
        }

        const auto account = QString::fromStdString(accountId);
        Q_EMIT messageMetadataChanged(account);
        Q_EMIT submitRequested(account);
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
        const auto result =
            m_mailService.queueSetEmailFlagged(*accountId, emailId.toStdString(), !flagged);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            Q_EMIT operationFailed(*error);
            return;
        }

        m_messageView.setCurrentIndex(index);
        const auto account = QString::fromStdString(*accountId);
        Q_EMIT messageMetadataChanged(account);
        Q_EMIT statusMessage(
            flagged ? QStringLiteral("Removed star.") : QStringLiteral("Added star."), 5000);
        Q_EMIT submitRequested(account);
    }

    void MessageCommandController::markSelectionUnread(std::optional<std::string> accountId,
                                                       std::optional<std::string> sourceMailboxId)
    {
        if (!accountId.has_value() || m_messageView.selectionModel() == nullptr)
        {
            Q_EMIT statusMessage(QStringLiteral("Select a message to mark unread."), 3000);
            return;
        }

        auto selection = selectedActionItems(true);
        if (selection.empty())
        {
            return;
        }

        qCInfo(logMessageCommands)
            << "mark unread requested" << selection.size() << "selection item(s)";
        const auto result = m_mailService.queueMarkMessagesUnread(
            *accountId, std::move(sourceMailboxId), std::move(selection));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            Q_EMIT operationFailed(*error);
            return;
        }

        const auto markedCount =
            std::get<javelin::app::QueuedMessageSelectionMutation>(result).queuedEmailCount;
        const auto account = QString::fromStdString(*accountId);
        Q_EMIT messageMetadataChanged(account);
        Q_EMIT statusMessage(markedCount == 1
                                 ? QStringLiteral("Marked unread.")
                                 : QStringLiteral("Marked %1 messages unread.").arg(markedCount),
                             5000);
        Q_EMIT submitRequested(account);
    }

    void MessageCommandController::queueArchive(std::string accountId,
                                                std::optional<std::string> sourceMailboxId,
                                                javelin::app::MessageSelection selection)
    {
        const bool searchArchive = !sourceMailboxId.has_value();
        const auto result = m_mailService.queueMailboxSelectionMutation(
            javelin::app::MailboxSelectionMutationIntent{
                .accountId = accountId,
                .selection = std::move(selection),
                .operation = javelin::app::MailboxSelectionOperation::Archive,
                .sourceMailboxId = std::move(sourceMailboxId),
                .destinationMailboxId = std::nullopt,
            });
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            Q_EMIT operationFailed(*error);
            return;
        }

        const auto& summary = std::get<javelin::app::QueuedMailboxSelectionMutation>(result);
        if (summary.queuedEmailCount == 0)
        {
            Q_EMIT statusMessage(
                searchArchive ? QStringLiteral("The selected messages are not in Inbox.")
                              : QStringLiteral("The selected messages are already archived."),
                5000);
            return;
        }

        Q_EMIT mailboxMembershipChanged(QString::fromStdString(accountId));
        if (summary.skippedEmailCount > 0)
        {
            Q_EMIT statusMessage(
                QStringLiteral("Queued archive for %1 messages; skipped %2 not in Inbox.")
                    .arg(summary.queuedEmailCount)
                    .arg(summary.skippedEmailCount),
                5000);
        }
        else
        {
            Q_EMIT statusMessage(summary.queuedEmailCount == 1
                                     ? QStringLiteral("Queued archive.")
                                     : QStringLiteral("Queued archive for %1 messages.")
                                           .arg(summary.queuedEmailCount),
                                 5000);
        }
        Q_EMIT submitRequested(QString::fromStdString(accountId));
    }

    void MessageCommandController::queueDelete(std::string accountId, std::string sourceMailboxId,
                                               javelin::app::MessageSelection selection)
    {
        const auto trashMailbox = findMailboxByRole(m_queryService, accountId, "trash");
        if (!trashMailbox.has_value())
        {
            Q_EMIT statusMessage(QStringLiteral("No Trash mailbox is available."), 5000);
            return;
        }
        queueTransfer(std::move(accountId), std::move(sourceMailboxId), trashMailbox->id,
                      std::move(selection), MessageTransferOperation::Move,
                      QStringLiteral("Queued delete."));
    }

    void MessageCommandController::queueDestroy(std::string accountId,
                                                std::optional<std::string> sourceMailboxId,
                                                javelin::app::MessageSelection selection)
    {
        qCInfo(logMessageCommands)
            << "permanently delete requested" << selection.size() << "selection item(s)";
        const auto result = m_mailService.queueDestroyMessages(
            accountId, std::move(sourceMailboxId), std::move(selection));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            Q_EMIT operationFailed(*error);
            return;
        }

        const auto selectedCount =
            std::get<javelin::app::QueuedMessageSelectionMutation>(result).queuedEmailCount;
        Q_EMIT mailboxMembershipChanged(QString::fromStdString(accountId));
        Q_EMIT statusMessage(
            selectedCount == 1
                ? QStringLiteral("Queued permanent deletion.")
                : QStringLiteral("Queued permanent deletion for %1 messages.").arg(selectedCount),
            5000);
        Q_EMIT submitRequested(QString::fromStdString(accountId));
    }

    bool
    MessageCommandController::confirmPermanentDelete(const std::size_t selectionItemCount) const
    {
        const auto prompt =
            selectionItemCount == 1
                ? QStringLiteral("Permanently delete the selected message? This cannot be undone.")
                : QStringLiteral("Permanently delete %1 selected messages? This cannot be undone.")
                      .arg(selectionItemCount);
        return QMessageBox::warning(m_dialogParent.data(), QStringLiteral("Delete Permanently"),
                                    prompt, QMessageBox::Yes | QMessageBox::Cancel,
                                    QMessageBox::Cancel) == QMessageBox::Yes;
    }

} // namespace javelin::gui::shell
