#include "app/undo/CalendarPreferenceExecutor.h"

namespace javelin::app::undo
{
    CalendarPreferenceExecutor::CalendarPreferenceExecutor(CalendarPreferencePort& preferences)
        : m_preferences(preferences)
    {
    }

    QCoro::Task<HistoryExecutionResult>
    CalendarPreferenceExecutor::execute(HistoryEntry entry,
                                        const HistoryExecutionDirection direction)
    {
        auto* history = std::get_if<CalendarPreferenceHistory>(&entry.payload);
        if (history == nullptr || direction == HistoryExecutionDirection::Recover)
            co_return HistoryExecutionResult{
                .outcome = HistoryExecutionOutcome::Conflict,
                .updatedPayload = std::nullopt,
                .refreshScope = {},
                .summary =
                    QStringLiteral("The calendar preference requires manual reconciliation."),
                .objectFailures = {},
                .mayRemoveFromHistory = true,
            };
        const bool undo = direction == HistoryExecutionDirection::Undo;
        const auto& expected = undo ? history->afterValue : history->beforeValue;
        const auto& desired = undo ? history->beforeValue : history->afterValue;
        const auto current = m_preferences.currentCalendarPreference(*history);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&current))
            co_return HistoryExecutionResult{
                .outcome = javelin::jmap::isTransientError(*error)
                               ? HistoryExecutionOutcome::Unknown
                               : HistoryExecutionOutcome::DefinitiveFailure,
                .updatedPayload = std::nullopt,
                .refreshScope = {},
                .summary = error->message,
                .objectFailures = {},
                .mayRemoveFromHistory = !javelin::jmap::isTransientError(*error),
            };
        if (std::get<std::optional<std::string>>(current) != expected)
            co_return HistoryExecutionResult{
                .outcome = HistoryExecutionOutcome::Conflict,
                .updatedPayload = std::nullopt,
                .refreshScope = {},
                .summary = QStringLiteral("The calendar preference changed after this action."),
                .objectFailures = {},
                .mayRemoveFromHistory = true,
            };
        if (const auto error = co_await m_preferences.applyCalendarPreference(
                *history, desired, undo ? CommandOrigin::Undo : CommandOrigin::Redo))
            co_return HistoryExecutionResult{
                .outcome = javelin::jmap::isTransientError(*error)
                               ? HistoryExecutionOutcome::Unknown
                               : HistoryExecutionOutcome::DefinitiveFailure,
                .updatedPayload = std::nullopt,
                .refreshScope = {},
                .summary = error->message,
                .objectFailures = {},
                .mayRemoveFromHistory = !javelin::jmap::isTransientError(*error),
            };
        co_return HistoryExecutionResult{
            .outcome = HistoryExecutionOutcome::Success,
            .updatedPayload = *history,
            .refreshScope =
                {
                    .accountIds = {QString::fromStdString(history->accountId)},
                    .objectTypes = {QStringLiteral("Calendar")},
                    .views = {QStringLiteral("calendar")},
                },
            .summary = {},
            .objectFailures = {},
            .mayRemoveFromHistory = false,
        };
    }
} // namespace javelin::app::undo
