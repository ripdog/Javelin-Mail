#include "app/undo/UndoManager.h"

#include "app/undo/HistorySerialization.h"

#include <QUuid>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace javelin::app::undo
{

    namespace
    {
        [[nodiscard]] std::size_t domainIndex(const HistoryDomain domain)
        {
            return static_cast<std::size_t>(domain);
        }

        [[nodiscard]] bool isBlocking(const HistoryEntryStatus status)
        {
            return status == HistoryEntryStatus::BlockedUnknown ||
                   status == HistoryEntryStatus::BlockedPartial;
        }

        [[nodiscard]] bool isExecuting(const HistoryEntryStatus status)
        {
            return status == HistoryEntryStatus::ExecutingForward ||
                   status == HistoryEntryStatus::ExecutingUndo ||
                   status == HistoryEntryStatus::ExecutingRedo;
        }

        [[nodiscard]] bool mayPrune(const HistoryEntry& entry)
        {
            if (isExecuting(entry.status) || isBlocking(entry.status))
                return false;
            return !std::holds_alternative<DeferredSendHistory>(entry.payload);
        }
    } // namespace

    UndoManager::UndoManager(HistoryRepository& repository, QObject* parent)
        : QObject(parent), m_repository(repository)
    {
    }

    void UndoManager::setExecutor(const HistoryDomain domain, HistoryCommandExecutor* executor)
    {
        m_executors.at(domainIndex(domain)) = executor;
        publishState();
    }

    void UndoManager::setExecutor(const QString& commandKind, HistoryCommandExecutor* executor)
    {
        if (executor == nullptr)
            m_commandExecutors.remove(commandKind);
        else
            m_commandExecutors.insert(commandKind, executor);
        publishState();
    }

    std::optional<javelin::jmap::cache::DatabaseError> UndoManager::load()
    {
        if (const auto error = reload())
            return error;
        if (const auto error = recoverInterruptedEntries())
            return error;
        if (const auto error = prune())
            return error;
        return reload();
    }

    std::variant<std::optional<HistoryEntry>, javelin::jmap::cache::DatabaseError>
    UndoManager::prepareNormal(QString label, const HistoryDomain domain, HistoryPayload payload,
                               std::optional<QString> operationGroupId,
                               std::optional<QDateTime> expiresAt, const CommandOrigin origin)
    {
        if (origin != CommandOrigin::User)
            return std::optional<HistoryEntry>{std::nullopt};

        HistoryEntry entry{
            .entryId = QUuid::createUuid().toString(QUuid::WithoutBraces),
            .stack = HistoryStack::Undo,
            .stackOrder = 0,
            .label = std::move(label),
            .domain = domain,
            .commandKind = payloadCommandKind(payload),
            .payloadVersion = 1,
            .payload = std::move(payload),
            .status = HistoryEntryStatus::Preparing,
            .operationGroupId = std::move(operationGroupId),
            .expiresAt = std::move(expiresAt),
            .explanation = std::nullopt,
            .failureJson = std::nullopt,
            .createdAt = {},
            .updatedAt = {},
        };
        auto result = m_repository.insertPreparing(std::move(entry));
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            return *error;
        if (const auto error = reload())
            return *error;
        return std::optional<HistoryEntry>{std::get<HistoryEntry>(std::move(result))};
    }

    std::variant<HistoryEntry, javelin::jmap::cache::DatabaseError>
    UndoManager::commitNormal(HistoryEntry entry)
    {
        auto result = m_repository.markPreparedReady(std::move(entry));
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            return *error;
        if (const auto error = prune())
            return *error;
        if (const auto error = reload())
            return *error;
        return std::get<HistoryEntry>(std::move(result));
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    UndoManager::discardNormal(const QString& entryId)
    {
        if (const auto error = m_repository.remove(entryId))
            return error;
        return reload();
    }

    std::optional<javelin::jmap::cache::DatabaseError> UndoManager::replaceEntry(HistoryEntry entry)
    {
        if (const auto error = m_repository.update(entry))
            return error;
        return reload();
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    UndoManager::setEntryStatus(const QString& entryId, const HistoryEntryStatus status,
                                std::optional<QString> failure)
    {
        const auto found = std::ranges::find(m_entries, entryId, &HistoryEntry::entryId);
        if (found == m_entries.end())
        {
            return javelin::jmap::cache::DatabaseError{
                .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Operation history entry does not exist."),
            };
        }
        auto entry = *found;
        entry.status = status;
        entry.failureJson = std::move(failure);
        return replaceEntry(std::move(entry));
    }

    std::variant<HistoryEntry, javelin::jmap::cache::DatabaseError>
    UndoManager::recordImpossible(QString label, const HistoryDomain domain, QString explanation,
                                  std::optional<QString> operationGroupId)
    {
        HistoryEntry entry{
            .entryId = QUuid::createUuid().toString(QUuid::WithoutBraces),
            .stack = HistoryStack::Undo,
            .stackOrder = 0,
            .label = std::move(label),
            .domain = domain,
            .commandKind = QStringLiteral("impossible"),
            .payloadVersion = 1,
            .payload = ImpossibleHistory{.explanation = explanation.toStdString()},
            .status = HistoryEntryStatus::Impossible,
            .operationGroupId = std::move(operationGroupId),
            .expiresAt = std::nullopt,
            .explanation = std::move(explanation),
            .failureJson = std::nullopt,
            .createdAt = {},
            .updatedAt = {},
        };
        auto result = m_repository.pushUndoClearingRedo(std::move(entry));
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            return *error;
        if (const auto error = prune())
            return *error;
        if (const auto error = reload())
            return *error;
        return std::get<HistoryEntry>(std::move(result));
    }

    QCoro::Task<bool> UndoManager::undo()
    {
        co_return co_await executeTop(HistoryStack::Undo);
    }

    QCoro::Task<bool> UndoManager::redo()
    {
        co_return co_await executeTop(HistoryStack::Redo);
    }

    QCoro::Task<bool> UndoManager::executeTop(const HistoryStack stack)
    {
        if (m_executing)
            co_return false;
        auto entry = top(stack);
        if (!entry.has_value() || isBlocking(entry->status))
            co_return false;

        if (entry->status == HistoryEntryStatus::Impossible ||
            entry->status == HistoryEntryStatus::Expired)
        {
            HistoryExecutionResult result{
                .outcome = entry->status == HistoryEntryStatus::Impossible
                               ? HistoryExecutionOutcome::Impossible
                               : HistoryExecutionOutcome::Expired,
                .updatedPayload = std::nullopt,
                .refreshScope = {},
                .summary = entry->explanation.value_or(
                    entry->status == HistoryEntryStatus::Impossible
                        ? QStringLiteral("This operation cannot be undone.")
                        : QStringLiteral("This operation has expired.")),
                .objectFailures = {},
                .mayRemoveFromHistory = true,
            };
            publishExecutionFailure(*entry, result, true);
            co_return false;
        }

        auto* executor = executorFor(*entry);
        if (executor == nullptr)
        {
            HistoryExecutionResult result{
                .outcome = HistoryExecutionOutcome::DefinitiveFailure,
                .updatedPayload = std::nullopt,
                .refreshScope = {},
                .summary = QStringLiteral("No executor is available for this operation."),
                .objectFailures = {},
                .mayRemoveFromHistory = false,
            };
            publishExecutionFailure(*entry, result);
            co_return false;
        }

        m_executing = true;
        entry->status = stack == HistoryStack::Undo ? HistoryEntryStatus::ExecutingUndo
                                                    : HistoryEntryStatus::ExecutingRedo;
        if (const auto error = m_repository.update(*entry))
        {
            m_executing = false;
            publishRepositoryFailure(*entry, QStringLiteral("Start history operation"), *error);
            co_return false;
        }
        static_cast<void>(reload());
        Q_EMIT executionStarted(entry->entryId);

        const auto direction = stack == HistoryStack::Undo ? HistoryExecutionDirection::Undo
                                                           : HistoryExecutionDirection::Redo;
        auto result = co_await executor->execute(*entry, direction);
        if (result.updatedPayload.has_value())
            entry->payload = std::move(*result.updatedPayload);

        if (result.outcome == HistoryExecutionOutcome::Success)
        {
            const auto destination =
                stack == HistoryStack::Undo ? HistoryStack::Redo : HistoryStack::Undo;
            auto moveResult = m_repository.move(*entry, destination, HistoryEntryStatus::Ready);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&moveResult))
            {
                m_executing = false;
                publishRepositoryFailure(*entry, QStringLiteral("Complete history operation"),
                                         *error);
                co_return false;
            }
            m_executing = false;
            static_cast<void>(reload());
            Q_EMIT executionCompleted(entry->entryId, result.refreshScope);
            co_return true;
        }

        switch (result.outcome)
        {
        case HistoryExecutionOutcome::Unknown:
            entry->status = HistoryEntryStatus::BlockedUnknown;
            break;
        case HistoryExecutionOutcome::PartialFailure:
            entry->status = HistoryEntryStatus::BlockedPartial;
            break;
        case HistoryExecutionOutcome::Impossible:
            entry->status = HistoryEntryStatus::Impossible;
            break;
        case HistoryExecutionOutcome::Expired:
            entry->status = HistoryEntryStatus::Expired;
            Q_EMIT entryExpired(entry->entryId);
            break;
        case HistoryExecutionOutcome::Conflict:
        case HistoryExecutionOutcome::DefinitiveFailure:
            entry->status = HistoryEntryStatus::Ready;
            break;
        case HistoryExecutionOutcome::Success:
            break;
        }
        entry->failureJson = result.summary;
        const auto updateError = m_repository.update(*entry);
        m_executing = false;
        static_cast<void>(reload());
        if (updateError.has_value())
        {
            publishRepositoryFailure(*entry, QStringLiteral("Retain failed history operation"),
                                     *updateError);
            co_return false;
        }
        publishExecutionFailure(*entry, result,
                                result.outcome == HistoryExecutionOutcome::Impossible ||
                                    result.outcome == HistoryExecutionOutcome::Expired);
        co_return false;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    UndoManager::acknowledgeAndRemove(const QString& entryId)
    {
        const auto found =
            std::find_if(m_entries.begin(), m_entries.end(),
                         [&](const HistoryEntry& entry) { return entry.entryId == entryId; });
        if (found == m_entries.end() || (found->status != HistoryEntryStatus::Impossible &&
                                         found->status != HistoryEntryStatus::Expired))
        {
            return javelin::jmap::cache::DatabaseError{
                .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Only impossible or expired history can be acknowledged"),
            };
        }
        return forget(entryId);
    }

    std::optional<javelin::jmap::cache::DatabaseError> UndoManager::forget(const QString& entryId)
    {
        if (m_executing)
        {
            return javelin::jmap::cache::DatabaseError{
                .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Cannot remove history while an operation is executing"),
            };
        }
        if (const auto error = m_repository.remove(entryId))
            return error;
        return reload();
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    UndoManager::forgetAndClearRedo(const QString& entryId)
    {
        if (m_executing)
            return javelin::jmap::cache::DatabaseError{
                .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Cannot branch history while an operation is executing"),
            };
        if (const auto error = m_repository.removeAndClearRedo(entryId))
            return error;
        return reload();
    }

    const HistoryState& UndoManager::state() const
    {
        return m_state;
    }

    const std::vector<HistoryEntry>& UndoManager::entries() const
    {
        return m_entries;
    }

    HistoryCommandExecutor* UndoManager::executorFor(const HistoryEntry& entry) const
    {
        const auto commandExecutor = m_commandExecutors.constFind(entry.commandKind);
        if (commandExecutor != m_commandExecutors.cend())
            return commandExecutor.value();
        return m_executors.at(domainIndex(entry.domain));
    }

    std::optional<HistoryEntry> UndoManager::top(const HistoryStack stack) const
    {
        const auto found =
            std::find_if(m_entries.begin(), m_entries.end(),
                         [stack](const HistoryEntry& entry) { return entry.stack == stack; });
        if (found == m_entries.end())
            return std::nullopt;
        return *found;
    }

    std::optional<javelin::jmap::cache::DatabaseError> UndoManager::reload()
    {
        auto result = m_repository.load();
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            return *error;
        m_entries = std::get<std::vector<HistoryEntry>>(std::move(result));
        publishState();
        return std::nullopt;
    }

    std::optional<javelin::jmap::cache::DatabaseError> UndoManager::recoverInterruptedEntries()
    {
        for (auto& entry : m_entries)
        {
            if (entry.status != HistoryEntryStatus::ExecutingUndo &&
                entry.status != HistoryEntryStatus::ExecutingRedo &&
                entry.status != HistoryEntryStatus::ExecutingForward)
            {
                continue;
            }
            entry.status = HistoryEntryStatus::BlockedUnknown;
            entry.failureJson =
                QStringLiteral("The application stopped while this operation was in flight.");
            if (const auto error = m_repository.update(entry))
                return error;
        }
        return std::nullopt;
    }

    std::optional<javelin::jmap::cache::DatabaseError> UndoManager::prune()
    {
        auto result = m_repository.load();
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            return *error;
        auto entries = std::get<std::vector<HistoryEntry>>(std::move(result));

        qsizetype payloadBytes = 0;
        for (const auto& entry : entries)
        {
            const auto serialized = serializeHistoryPayload(entry.payload);
            if (const auto* payload = std::get_if<SerializedHistoryPayload>(&serialized))
                payloadBytes += payload->json.toUtf8().size();
        }

        std::ranges::sort(entries, {}, &HistoryEntry::stackOrder);
        auto count = entries.size();
        for (const auto& entry : entries)
        {
            if (count <= maxEntryCount && payloadBytes <= maxPayloadBytes)
                break;
            if (!mayPrune(entry))
                continue;
            const auto serialized = serializeHistoryPayload(entry.payload);
            if (const auto* payload = std::get_if<SerializedHistoryPayload>(&serialized))
                payloadBytes -= payload->json.toUtf8().size();
            if (const auto error = m_repository.remove(entry.entryId))
                return error;
            --count;
        }
        return std::nullopt;
    }

    void UndoManager::publishState()
    {
        const auto undoEntry = top(HistoryStack::Undo);
        const auto redoEntry = top(HistoryStack::Redo);
        const bool blocked = (undoEntry.has_value() && isBlocking(undoEntry->status)) ||
                             (redoEntry.has_value() && isBlocking(redoEntry->status));
        HistoryState next{
            .undoLabel = undoEntry.has_value() ? QStringLiteral("Undo ") + undoEntry->label
                                               : QStringLiteral("Undo"),
            .redoLabel = redoEntry.has_value() ? QStringLiteral("Redo ") + redoEntry->label
                                               : QStringLiteral("Redo"),
            .canUndo = undoEntry.has_value() && !m_executing && !blocked,
            .canRedo = redoEntry.has_value() && !m_executing && !blocked,
            .executing = m_executing,
            .blocked = blocked,
        };
        if (next.undoLabel == m_state.undoLabel && next.redoLabel == m_state.redoLabel &&
            next.canUndo == m_state.canUndo && next.canRedo == m_state.canRedo &&
            next.executing == m_state.executing && next.blocked == m_state.blocked)
        {
            return;
        }
        m_state = std::move(next);
        Q_EMIT historyStateChanged(m_state);
    }

    void UndoManager::publishRepositoryFailure(const HistoryEntry& entry, const QString& operation,
                                               const javelin::jmap::cache::DatabaseError& error)
    {
        Q_EMIT executionFailed(HistoryFailure{
            .entryId = entry.entryId,
            .actionLabel = entry.label,
            .summary = operation + QStringLiteral(": ") + error.message,
            .objectFailures = {},
            .mayRemoveFromHistory = false,
            .acknowledgeAndRemove = false,
        });
    }

    void UndoManager::publishExecutionFailure(const HistoryEntry& entry,
                                              const HistoryExecutionResult& result,
                                              const bool acknowledgeAndRemove)
    {
        Q_EMIT executionFailed(HistoryFailure{
            .entryId = entry.entryId,
            .actionLabel = entry.label,
            .summary = result.summary,
            .objectFailures = result.objectFailures,
            .mayRemoveFromHistory = result.mayRemoveFromHistory,
            .acknowledgeAndRemove = acknowledgeAndRemove,
        });
    }

} // namespace javelin::app::undo
