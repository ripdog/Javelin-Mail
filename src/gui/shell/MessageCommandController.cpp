#include "gui/shell/MessageCommandController.h"

#include "gui/mailboxes/MailboxPresentation.h"
#include "gui/messages/MessageActionSelection.h"
#include "gui/messages/MessageListModel.h"
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
#include <QPalette>
#include <QPersistentModelIndex>

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
        javelin::jmap::cache::MailboxReader& mailboxReader, QListView& messageView,
        QWidget* dialogParent, QObject* parent)
        : QObject(parent), m_mailCommandPort(mailCommandPort), m_mailboxReader(mailboxReader),
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
        if (!accountId.has_value() || !sourceMailboxId.has_value())
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
        populateDestinationMenus(move ? &menu : nullptr, move ? nullptr : &menu,
                                 std::move(*accountId), std::move(sourceMailboxId),
                                 std::move(selection));
        if (menu.actions().empty())
        {
            Q_EMIT statusMessage(i18n("No destination mailboxes available."), 3000);
            return;
        }
        menu.exec(QCursor::pos());
    }

    void MessageCommandController::populateDestinationMenus(
        QMenu* moveMenu, QMenu* copyMenu, std::string accountId,
        std::optional<std::string> sourceMailboxId, javelin::app::MessageSelection selection)
    {
        const auto mailboxesResult = m_mailboxReader.listMailboxTree(accountId);
        const auto* mailboxes =
            std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&mailboxesResult);
        if (mailboxes == nullptr)
        {
            return;
        }

        const auto presentation =
            javelin::gui::mailboxes::buildMailboxPresentation(accountId, *mailboxes);
        const auto addDestinations = [this, &accountId, &sourceMailboxId, &selection,
                                      &presentation](QMenu* menu,
                                                     const MessageTransferOperation operation,
                                                     const QString& successMessage)
        {
            if (menu == nullptr)
                return;

            bool displayedSpecialUse = false;
            bool insertedUserSeparator = false;
            const auto iconColor = menu->palette().color(QPalette::Active, QPalette::Text);
            for (const auto& row :
                 javelin::gui::mailboxes::flattenMailboxPresentation(presentation))
            {
                const auto& destination = *row.node;
                if (!destination.mailbox.myRights.mayAddItems)
                    continue;
                if (destination.group == javelin::gui::mailboxes::MailboxPresentationGroup::User &&
                    displayedSpecialUse && !insertedUserSeparator)
                {
                    menu->addSeparator();
                    insertedUserSeparator = true;
                }

                const QString indentation(static_cast<qsizetype>(row.depth), QChar{u'\u2003'});
                auto* action =
                    menu->addAction(javelin::gui::mailboxes::mailboxPresentationIcon(
                                        destination.mailbox.role, iconColor),
                                    indentation + QString::fromStdString(destination.mailbox.name));
                connect(action, &QAction::triggered, this,
                        [this, accountId, sourceMailboxId,
                         destinationMailboxId = destination.mailbox.id, selection, operation,
                         successMessage]
                        {
                            queueTransfer(accountId, sourceMailboxId, destinationMailboxId,
                                          selection, operation, successMessage);
                        });
                displayedSpecialUse =
                    displayedSpecialUse ||
                    destination.group ==
                        javelin::gui::mailboxes::MailboxPresentationGroup::SpecialUse;
            }
        };

        // The open mailbox scopes collapsed-thread resolution; it does not describe the
        // residency of every visible or selected Email. Keep every writable mailbox available so
        // mixed selections can copy or move the members that are not already in that destination.
        addDestinations(moveMenu, MessageTransferOperation::Move, i18n("Queued move."));
        addDestinations(copyMenu, MessageTransferOperation::Copy, i18n("Queued copy."));
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
        auto task = m_mailCommandPort.queueMailboxSelectionMutation(
            javelin::app::MailboxSelectionMutationIntent{
                .accountId = accountId,
                .selection = std::move(selection),
                .operation = move ? javelin::app::MailboxSelectionOperation::Move
                                  : javelin::app::MailboxSelectionOperation::Copy,
                .sourceMailboxId = std::move(sourceMailboxId),
                .destinationMailboxId = std::move(destinationMailboxId),
            });
        QCoro::connect(
            std::move(task), this,
            [this, accountId = std::move(accountId), move,
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

                Q_EMIT mailboxMembershipChanged(QString::fromStdString(accountId));
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
            [this, accountId = std::move(accountId), emailId = std::move(emailId)](
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
                Q_EMIT emailMarkedRead(QString::fromStdString(accountId),
                                       QString::fromStdString(emailId));
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
                Q_EMIT messageMetadataChanged(QString::fromStdString(accountId));
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
                Q_EMIT messageMetadataChanged(QString::fromStdString(accountId));
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
                Q_EMIT messageMetadataChanged(QString::fromStdString(accountId));
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
                Q_EMIT messageMetadataChanged(QString::fromStdString(accountId));
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

                Q_EMIT junkStateChanged(QString::fromStdString(accountId));
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

                Q_EMIT mailboxMembershipChanged(QString::fromStdString(accountId));
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
        queueTransfer(std::move(accountId), std::move(sourceMailboxId), trashMailbox->id,
                      std::move(selection), MessageTransferOperation::Move, i18n("Queued delete."));
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
                Q_EMIT mailboxMembershipChanged(QString::fromStdString(accountId));
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

    bool
    MessageCommandController::confirmPermanentDelete(const std::size_t selectionItemCount) const
    {
        const auto prompt =
            selectionItemCount == 1
                ? i18n("Permanently delete the selected message? This cannot be undone.")
                : i18np("Permanently delete %1 selected message? This cannot be undone.",
                        "Permanently delete %1 selected messages? This cannot be undone.",
                        selectionItemCount);
        return QMessageBox::warning(m_dialogParent.data(), i18n("Delete Permanently"), prompt,
                                    QMessageBox::Yes | QMessageBox::Cancel,
                                    QMessageBox::Cancel) == QMessageBox::Yes;
    }

} // namespace javelin::gui::shell
