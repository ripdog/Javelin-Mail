#include "app/undo/MailTransferHistoryCoordinator.h"

#include "app/MailTransferRepository.h"
#include "app/undo/HistoryTypes.h"
#include "app/undo/UndoManager.h"
#include "jmap/cache/EmailRepository.h"

#include <KLocalizedString>

#include <ranges>
#include <utility>
#include <vector>

namespace javelin::app::undo
{
    namespace
    {
        using DatabaseError = javelin::jmap::cache::DatabaseError;
        using OperationError = javelin::jmap::OperationError;
        using OperationErrorCode = javelin::jmap::OperationErrorCode;

        [[nodiscard]] OperationError invalidHistory(QString message)
        {
            return {.code = OperationErrorCode::PreconditionFailed, .message = std::move(message)};
        }

        [[nodiscard]] std::optional<HistoryEntry>
        matchingHistory(const UndoManager& manager, const QString& operationGroupId)
        {
            const auto found = std::ranges::find_if(
                manager.entries(),
                [&](const HistoryEntry& entry)
                {
                    return entry.operationGroupId == std::optional<QString>{operationGroupId} &&
                           std::holds_alternative<MailTransferHistory>(entry.payload);
                });
            if (found == manager.entries().end())
                return std::nullopt;
            return *found;
        }
    } // namespace

    MailTransferHistoryCoordinator::MailTransferHistoryCoordinator(
        javelin::jmap::cache::DatabaseConnection& databaseConnection, UndoManager& undoManager)
        : m_databaseConnection(databaseConnection), m_undoManager(undoManager)
    {
    }

    MailTransferHistoryFinalizationResult
    MailTransferHistoryCoordinator::finalizeCompleted(std::string operationId)
    {
        javelin::app::MailTransferRepository transfers{m_databaseConnection};
        const auto operationResult = transfers.findOperation(operationId);
        if (const auto* error = std::get_if<DatabaseError>(&operationResult))
            return javelin::jmap::operationError(*error);
        const auto& maybeOperation =
            std::get<std::optional<javelin::app::MailTransferOperationRecord>>(operationResult);
        if (!maybeOperation.has_value())
            return invalidHistory(i18n("The mail transfer no longer exists."));
        const auto& operation = *maybeOperation;
        if (operation.status != javelin::app::MailTransferStatus::Complete)
            return invalidHistory(i18n("Only a completed mail transfer can be added to Undo history."));

        if (operation.historyEntryId.has_value())
        {
            const auto found = std::ranges::find(m_undoManager.entries(), *operation.historyEntryId,
                                                 &HistoryEntry::entryId);
            if (found == m_undoManager.entries().end())
            {
                // The entry was deliberately pruned or forgotten. The durable marker prevents
                // startup recovery from resurrecting old Undo history.
                return std::optional<QString>{std::nullopt};
            }
            if (!std::holds_alternative<MailTransferHistory>(found->payload))
                return invalidHistory(i18n("The transfer history marker refers to another command."));
            return std::optional<QString>{found->entryId};
        }

        const auto itemsResult = transfers.listItems(operation.operationId);
        if (const auto* error = std::get_if<DatabaseError>(&itemsResult))
            return javelin::jmap::operationError(*error);
        const auto& items = std::get<std::vector<javelin::app::MailTransferItemRecord>>(itemsResult);
        if (items.empty())
            return invalidHistory(i18n("The completed mail transfer has no messages."));
        if (std::ranges::any_of(items, [](const auto& item)
                               { return item.phase != javelin::app::MailTransferItemPhase::Complete; }))
        {
            return invalidHistory(i18n("The completed mail transfer contains an unfinished message."));
        }

        const QString operationGroupId = QString::fromStdString(
            operation.operationGroupId.value_or(operation.operationId));
        auto historyEntry = matchingHistory(m_undoManager, operationGroupId);
        if (!historyEntry.has_value())
        {
            MailTransferHistory history{
                .sourceAccountId = operation.sourceAccountId,
                .destinationAccountId = operation.destinationAccountId,
                .destinationMailboxId = operation.destinationMailboxId,
                .operation = operation.operation == javelin::app::MailTransferOperation::Move
                                 ? MailTransferHistoryOperation::Move
                                 : MailTransferHistoryOperation::Copy,
                .items = {},
            };
            history.items.reserve(items.size());

            javelin::jmap::cache::EmailRepository emails{m_databaseConnection};
            for (const auto& item : items)
            {
                if (!item.destinationEmailId.has_value())
                    return invalidHistory(
                        i18n("A completed transfer is missing its destination Email id."));
                if (item.sourceDestroy && !item.rawContentHash.has_value())
                    return invalidHistory(
                        i18n("A destructive move is missing its retained raw message source."));
                if (item.reusedExisting && !item.destinationPriorMailboxIds.has_value())
                    return invalidHistory(
                        i18n("A reused destination message is missing its prior mailbox snapshot."));

                const auto destinationResult =
                    emails.find(operation.destinationAccountId, *item.destinationEmailId);
                if (const auto* error = std::get_if<DatabaseError>(&destinationResult))
                    return javelin::jmap::operationError(*error);
                const auto& destination =
                    std::get<std::optional<javelin::jmap::domain::Email>>(destinationResult);
                if (!destination.has_value())
                {
                    return invalidHistory(i18n(
                        "The completed transfer destination is not materialized in the local cache."));
                }

                history.items.push_back(MailTransferItemHistory{
                    .currentSourceEmailId = item.sourceDestroy
                                                ? std::nullopt
                                                : std::optional<std::string>{item.sourceEmailId},
                    .originalSourceMailboxIds = item.sourceMailboxIds,
                    .sourceKeywords = item.sourceKeywords,
                    .sourceReceivedAt = item.sourceReceivedAt,
                    .sourceSize = item.sourceSize,
                    .sourceRemovedMailboxIds = item.sourceRemoveMailboxIds,
                    .sourceDestroyed = item.sourceDestroy,
                    .rawContentHash = item.rawContentHash,
                    .currentDestinationEmailId = item.destinationEmailId,
                    .destinationReusedExisting = item.reusedExisting,
                    .destinationPriorMailboxIds =
                        item.destinationPriorMailboxIds.value_or(std::vector<std::string>{}),
                    .destinationMailboxIds = destination->mailboxIds,
                    .destinationKeywords = destination->keywords,
                });
            }

            auto recorded = m_undoManager.recordNormal(
                operation.title, HistoryDomain::Mail, std::move(history), operationGroupId);
            if (const auto* error = std::get_if<DatabaseError>(&recorded))
                return javelin::jmap::operationError(*error);
            historyEntry = std::get<HistoryEntry>(std::move(recorded));
        }

        for (const auto& item : items)
        {
            if (!item.sourceDestroy)
                continue;
            const auto reassigned = transfers.reassignSourcePin(
                item.itemId, "history_entry", historyEntry->entryId.toStdString());
            if (const auto* error = std::get_if<DatabaseError>(&reassigned))
                return javelin::jmap::operationError(*error);
            if (!std::get<bool>(reassigned))
            {
                return OperationError{
                    .code = OperationErrorCode::LocalStorageFailure,
                    .message = i18n("The retained source message could not be handed to Undo history."),
                };
            }
        }

        const auto marked = transfers.markHistoryPublished(operation.operationId, historyEntry->entryId);
        if (const auto* error = std::get_if<DatabaseError>(&marked))
            return javelin::jmap::operationError(*error);
        if (!std::get<bool>(marked))
        {
            return OperationError{
                .code = OperationErrorCode::Conflict,
                .message = i18n("The mail transfer history marker changed during finalization."),
            };
        }
        return std::optional<QString>{historyEntry->entryId};
    }

} // namespace javelin::app::undo
