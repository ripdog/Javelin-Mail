#include "app/undo/CalendarHistoryExecutor.h"

#include "jmap/api/CalendarMethods.h"

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

        [[nodiscard]] HistoryExecutionResult success(CalendarEventHistory history)
        {
            const auto accountId = QString::fromStdString(history.accountId);
            return {
                .outcome = HistoryExecutionOutcome::Success,
                .updatedPayload = std::move(history),
                .refreshScope =
                    {
                        .accountIds = {accountId},
                        .objectTypes = {QStringLiteral("CalendarEvent")},
                        .views = {QStringLiteral("calendar")},
                    },
                .summary = {},
                .objectFailures = {},
                .mayRemoveFromHistory = false,
            };
        }

        [[nodiscard]] std::optional<javelin::jmap::calendar::CalendarEvent>
        parseEvent(const CalendarEventHistory& history, const std::optional<std::string>& document)
        {
            if (!document.has_value())
                return std::nullopt;
            const auto parsed =
                javelin::jmap::api::parseCalendarEventDocument(history.accountId, *document);
            return parsed.value;
        }

        [[nodiscard]] bool sameEvent(javelin::jmap::calendar::CalendarEvent left,
                                     javelin::jmap::calendar::CalendarEvent right,
                                     const std::string& currentId)
        {
            left.id = currentId;
            right.id = currentId;
            left.baseEventId = std::nullopt;
            right.baseEventId = std::nullopt;
            return left == right;
        }

        [[nodiscard]] std::vector<std::string>
        calendarIds(const javelin::jmap::calendar::CalendarEvent& event)
        {
            std::vector<std::string> result;
            for (const auto& [calendarId, present] : event.calendarIds)
                if (present)
                    result.push_back(calendarId);
            return result;
        }
    } // namespace

    CalendarHistoryExecutor::CalendarHistoryExecutor(CalendarHistoryPort& calendarService)
        : m_calendarService(calendarService)
    {
    }

    QCoro::Task<HistoryExecutionResult>
    CalendarHistoryExecutor::execute(HistoryEntry entry, const HistoryExecutionDirection direction)
    {
        auto* history = std::get_if<CalendarEventHistory>(&entry.payload);
        if (history == nullptr || direction == HistoryExecutionDirection::Recover)
            co_return conflict(
                QStringLiteral("The calendar operation requires authoritative reconciliation."));

        const bool undo = direction == HistoryExecutionDirection::Undo;
        const auto expected =
            parseEvent(*history, undo ? history->afterDocumentJson : history->beforeDocumentJson);
        auto desired =
            parseEvent(*history, undo ? history->beforeDocumentJson : history->afterDocumentJson);
        if ((undo ? history->afterDocumentJson : history->beforeDocumentJson).has_value() !=
                expected.has_value() ||
            (undo ? history->beforeDocumentJson : history->afterDocumentJson).has_value() !=
                desired.has_value())
            co_return conflict(QStringLiteral("The calendar history payload is invalid."));

        javelin::jmap::calendar::AuthoritativeCalendarEvent authoritative{
            .state = {},
            .event = std::nullopt,
        };
        auto loaded = co_await m_calendarService.getAuthoritativeCalendarEvent(
            history->connectionId, history->accountId, history->currentEventId, history->uid);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&loaded))
            co_return failure(*error);
        authoritative =
            std::get<javelin::jmap::calendar::AuthoritativeCalendarEvent>(std::move(loaded));

        if (expected.has_value())
        {
            if (!authoritative.event.has_value() || !history->currentEventId.has_value())
                co_return conflict(
                    QStringLiteral("The calendar event is no longer available on the server."));
            if (!sameEvent(*authoritative.event, *expected, *history->currentEventId))
                co_return conflict(QStringLiteral("The calendar event changed on another client."));
        }
        else if (authoritative.event.has_value())
            co_return conflict(
                QStringLiteral("A calendar event exists where this history expected none."));

        const auto origin = undo ? CommandOrigin::Undo : CommandOrigin::Redo;
        if (!desired.has_value())
        {
            if (!history->currentEventId.has_value())
                co_return conflict(QStringLiteral("The calendar event identity is unavailable."));
            auto removed = co_await m_calendarService.deleteCalendarEvent(
                history->connectionId,
                {
                    .accountId = history->accountId,
                    .eventId = *history->currentEventId,
                    .calendarIds = calendarIds(*expected),
                    .operationGroupId = entry.operationGroupId
                                            ? std::optional{entry.operationGroupId->toStdString()}
                                            : std::nullopt,
                    .ifInState = authoritative.state,
                },
                origin);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&removed))
                co_return failure(*error);
            history->currentEventId = std::nullopt;
            co_return success(*history);
        }

        desired->accountId = history->accountId;
        if (authoritative.event.has_value())
        {
            desired->id = *history->currentEventId;
            auto updated = co_await m_calendarService.updateCalendarEvent(
                history->connectionId,
                {
                    .accountId = history->accountId,
                    .event = std::move(*desired),
                    .operationGroupId = entry.operationGroupId
                                            ? std::optional{entry.operationGroupId->toStdString()}
                                            : std::nullopt,
                    .ifInState = authoritative.state,
                },
                origin);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&updated))
                co_return failure(*error);
        }
        else
        {
            desired->id.clear();
            auto created = co_await m_calendarService.createCalendarEvent(
                history->connectionId,
                {
                    .accountId = history->accountId,
                    .event = std::move(*desired),
                    .operationGroupId = entry.operationGroupId
                                            ? std::optional{entry.operationGroupId->toStdString()}
                                            : std::nullopt,
                    .ifInState = authoritative.state.empty() ? std::nullopt
                                                             : std::optional{authoritative.state},
                },
                origin);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&created))
                co_return failure(*error);
            const auto& summary = std::get<javelin::jmap::calendar::CommittedMutation>(created);
            if (!summary.createdId.has_value())
                co_return conflict(
                    QStringLiteral("The recreated calendar event has no server identity."));
            history->currentEventId = summary.createdId;
        }
        co_return success(*history);
    }
} // namespace javelin::app::undo
