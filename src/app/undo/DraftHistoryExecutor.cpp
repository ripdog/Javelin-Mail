#include "app/undo/DraftHistoryExecutor.h"

#include "jmap/submission/DraftSnapshotSerialization.h"

#include <utility>

namespace javelin::app::undo
{
    namespace
    {
        [[nodiscard]] HistoryExecutionOutcome outcomeFor(const javelin::jmap::OperationError& error)
        {
            using enum javelin::jmap::OperationErrorCode;
            if (error.code == Conflict || error.code == PreconditionFailed ||
                error.code == NotFound)
                return HistoryExecutionOutcome::Conflict;
            if (javelin::jmap::isTransientError(error))
                return HistoryExecutionOutcome::Unknown;
            return HistoryExecutionOutcome::DefinitiveFailure;
        }

        [[nodiscard]] HistoryExecutionResult failure(const javelin::jmap::OperationError& error)
        {
            return {
                .outcome = outcomeFor(error),
                .updatedPayload = std::nullopt,
                .refreshScope = {},
                .summary = error.message,
                .objectFailures = {},
                .mayRemoveFromHistory = !javelin::jmap::isTransientError(error),
            };
        }

        [[nodiscard]] HistoryExecutionResult conflict(QString message)
        {
            return {
                .outcome = HistoryExecutionOutcome::Conflict,
                .updatedPayload = std::nullopt,
                .refreshScope = {},
                .summary = std::move(message),
                .objectFailures = {},
                .mayRemoveFromHistory = true,
            };
        }

        [[nodiscard]] javelin::jmap::submission::DraftSnapshot
        normalized(javelin::jmap::submission::DraftSnapshot snapshot,
                   const std::string& composeSessionId,
                   const std::optional<std::string>& currentDraftEmailId)
        {
            snapshot.composeSessionId = composeSessionId;
            snapshot.draftEmailId = currentDraftEmailId;
            for (auto& attachment : snapshot.attachments)
                attachment.localFilePath.clear();
            return snapshot;
        }

        [[nodiscard]] bool sameSnapshot(javelin::jmap::submission::DraftSnapshot left,
                                        javelin::jmap::submission::DraftSnapshot right,
                                        const DraftHistory& history)
        {
            left =
                normalized(std::move(left), history.composeSessionId, history.currentDraftEmailId);
            right =
                normalized(std::move(right), history.composeSessionId, history.currentDraftEmailId);
            return javelin::jmap::submission::serializeDraftSnapshot(left) ==
                   javelin::jmap::submission::serializeDraftSnapshot(right);
        }

        [[nodiscard]] HistoryExecutionResult success(DraftHistory history)
        {
            const auto accountId = QString::fromStdString(history.accountId);
            return {
                .outcome = HistoryExecutionOutcome::Success,
                .updatedPayload = std::move(history),
                .refreshScope =
                    {
                        .accountIds = {accountId},
                        .objectTypes = {QStringLiteral("Email")},
                        .views = {QStringLiteral("drafts"), QStringLiteral("compose")},
                    },
                .summary = {},
                .objectFailures = {},
                .mayRemoveFromHistory = false,
            };
        }
    } // namespace

    DraftHistoryExecutor::DraftHistoryExecutor(DraftHistoryPort& composeService)
        : m_composeService(composeService)
    {
    }

    QCoro::Task<HistoryExecutionResult>
    DraftHistoryExecutor::execute(HistoryEntry entry, const HistoryExecutionDirection direction)
    {
        auto* history = std::get_if<DraftHistory>(&entry.payload);
        if (history == nullptr || direction == HistoryExecutionDirection::Recover)
            co_return conflict(
                QStringLiteral("The previous draft operation requires reconciliation."));

        const bool undo = direction == HistoryExecutionDirection::Undo;
        const std::optional<std::string> expectedJson =
            undo ? std::optional{history->afterSnapshotJson} : history->beforeSnapshotJson;
        const std::optional<std::string> desiredJson =
            undo ? history->beforeSnapshotJson : std::optional{history->afterSnapshotJson};

        std::optional<javelin::jmap::submission::DraftSnapshot> authoritative;
        if (history->currentDraftEmailId.has_value())
        {
            auto loaded = co_await m_composeService.loadAuthoritativeDraft(
                history->accountId, *history->currentDraftEmailId, history->composeSessionId);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&loaded))
                co_return failure(*error);
            authoritative = std::get<javelin::jmap::submission::DraftSnapshot>(std::move(loaded));
        }

        if (expectedJson.has_value())
        {
            if (!authoritative.has_value())
                co_return conflict(
                    QStringLiteral("The saved draft is no longer available on the server."));
            const auto expected = javelin::jmap::submission::deserializeDraftSnapshot(
                QString::fromStdString(*expectedJson));
            if (!expected.has_value() || !sameSnapshot(*authoritative, *expected, *history))
                co_return conflict(QStringLiteral("The saved draft changed on another client."));
        }
        else if (authoritative.has_value())
        {
            co_return conflict(
                QStringLiteral("A saved draft exists where this history expected none."));
        }

        const auto origin = undo ? CommandOrigin::Undo : CommandOrigin::Redo;
        if (!desiredJson.has_value())
        {
            auto removed = co_await m_composeService.deleteDraftFromHistory(
                history->accountId, *history->currentDraftEmailId, origin);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&removed))
                co_return failure(*error);
            history->currentDraftEmailId = std::nullopt;
            co_return success(*history);
        }

        auto desired = javelin::jmap::submission::deserializeDraftSnapshot(
            QString::fromStdString(*desiredJson));
        if (!desired.has_value())
            co_return conflict(QStringLiteral("The saved draft history payload is invalid."));
        desired->composeSessionId = history->composeSessionId;
        desired->draftEmailId = history->currentDraftEmailId;
        auto saved = co_await m_composeService.saveDraftFromHistory(std::move(*desired), origin);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&saved))
            co_return failure(*error);
        const auto& summary = std::get<javelin::jmap::submission::DraftSaveSummary>(saved);
        history->currentDraftEmailId = summary.draftEmailId;
        co_return success(*history);
    }

} // namespace javelin::app::undo
