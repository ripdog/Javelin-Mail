#include "app/undo/CalendarHistoryExecutor.h"

#include "jmap/api/CalendarMethods.h"

#include <QCoroTask>

#include <catch2/catch_test_macros.hpp>

namespace
{
    using namespace javelin::app::undo;
    using namespace javelin::jmap::calendar;

    class FakeCalendarHistoryPort final : public CalendarHistoryPort
    {
      public:
        std::optional<CalendarEvent> current;
        std::string state = "state-1";
        int nextId = 1;
        int mutations = 0;

        QCoro::Task<AuthoritativeCalendarEventResult>
        getAuthoritativeCalendarEvent(std::string, std::string, std::optional<std::string> eventId,
                                      std::string uid) override
        {
            std::optional<CalendarEvent> found;
            if (current.has_value() && current->uid == uid &&
                (!eventId.has_value() || current->id == *eventId))
                found = current;
            co_return AuthoritativeCalendarEvent{.state = state, .event = std::move(found)};
        }

        QCoro::Task<CalendarMutationResult>
        createCalendarEvent(std::string, CreateEventCommand command, CommandOrigin) override
        {
            ++mutations;
            command.event.id = "event-" + std::to_string(nextId++);
            current = command.event;
            state = "state-created";
            co_return CommittedMutation{
                .accountId = command.accountId,
                .newState = state,
                .createdId = command.event.id,
            };
        }

        QCoro::Task<CalendarMutationResult>
        updateCalendarEvent(std::string, UpdateEventCommand command, CommandOrigin) override
        {
            ++mutations;
            current = command.event;
            state = "state-updated";
            co_return CommittedMutation{
                .accountId = command.accountId,
                .newState = state,
                .createdId = std::nullopt,
            };
        }

        QCoro::Task<CalendarMutationResult>
        deleteCalendarEvent(std::string, DeleteEventCommand command, CommandOrigin) override
        {
            ++mutations;
            current = std::nullopt;
            state = "state-deleted";
            co_return CommittedMutation{
                .accountId = command.accountId,
                .newState = state,
                .createdId = std::nullopt,
            };
        }
    };

    [[nodiscard]] CalendarEvent event(std::string id, std::string title)
    {
        return {
            .accountId = "account-1",
            .id = std::move(id),
            .baseEventId = std::nullopt,
            .recurrenceId = std::nullopt,
            .uid = "uid-1",
            .calendarIds = {{"calendar-1", true}},
            .title = std::move(title),
            .description = std::nullopt,
            .location = std::nullopt,
            .start = {.value = "2026-08-01T09:00:00"},
            .duration = {.value = "PT1H"},
            .timeZone = TimeZoneId{.value = "Pacific/Auckland"},
            .showWithoutTime = false,
            .isDraft = false,
            .isOrigin = true,
            .useDefaultAlerts = false,
            .alerts = {},
            .utcStart = std::nullopt,
            .utcEnd = std::nullopt,
            .recurrenceRule = std::nullopt,
            .recurrenceOverrides = {},
            .attendees = {},
        };
    }

    [[nodiscard]] std::optional<std::string> document(const std::optional<CalendarEvent>& value)
    {
        if (!value.has_value())
            return std::nullopt;
        return javelin::jmap::api::serializeCalendarEventDocument(*value);
    }

    [[nodiscard]] HistoryEntry entry(const std::optional<CalendarEvent>& before,
                                     const std::optional<CalendarEvent>& after,
                                     std::optional<std::string> currentId)
    {
        HistoryEntry value;
        value.entryId = QStringLiteral("calendar-history");
        value.label = QStringLiteral("Edit “Appointment”");
        value.domain = HistoryDomain::Calendar;
        value.commandKind = QStringLiteral("calendar_event");
        value.payload = CalendarEventHistory{
            .connectionId = "owner-1",
            .accountId = "account-1",
            .calendarId = "calendar-1",
            .currentEventId = std::move(currentId),
            .uid = "uid-1",
            .beforeDocumentJson = document(before),
            .afterDocumentJson = document(after),
        };
        value.status = HistoryEntryStatus::Ready;
        value.operationGroupId = QStringLiteral("group-1");
        return value;
    }
} // namespace

TEST_CASE("calendar create undo deletes and redo recreates with a remapped id",
          "[app][undo][calendar-executor]")
{
    FakeCalendarHistoryPort port;
    const auto created = event("event-original", "Appointment");
    port.current = created;
    CalendarHistoryExecutor executor{port};

    auto undone = QCoro::waitFor(executor.execute(entry(std::nullopt, created, created.id),
                                                  HistoryExecutionDirection::Undo));
    CHECK(undone.outcome == HistoryExecutionOutcome::Success);
    CHECK_FALSE(port.current.has_value());

    auto redoEntry = entry(std::nullopt, created, created.id);
    redoEntry.payload = *undone.updatedPayload;
    auto redone =
        QCoro::waitFor(executor.execute(std::move(redoEntry), HistoryExecutionDirection::Redo));
    CHECK(redone.outcome == HistoryExecutionOutcome::Success);
    REQUIRE(port.current.has_value());
    CHECK(port.current->uid == "uid-1");
    CHECK(std::get<CalendarEventHistory>(*redone.updatedPayload).currentEventId ==
          std::optional<std::string>{"event-1"});
}

TEST_CASE("calendar delete undo recreates the complete event", "[app][undo][calendar-executor]")
{
    FakeCalendarHistoryPort port;
    const auto removed = event("event-old", "Dentist appointment");
    CalendarHistoryExecutor executor{port};

    const auto result = QCoro::waitFor(executor.execute(entry(removed, std::nullopt, removed.id),
                                                        HistoryExecutionDirection::Undo));
    CHECK(result.outcome == HistoryExecutionOutcome::Success);
    REQUIRE(port.current.has_value());
    CHECK(port.current->title == "Dentist appointment");
    CHECK(port.current->uid == removed.uid);
    CHECK(port.current->id == "event-1");
}

TEST_CASE("calendar undo refuses to overwrite an external edit", "[app][undo][calendar-executor]")
{
    FakeCalendarHistoryPort port;
    const auto before = event("event-1", "Before");
    const auto after = event("event-1", "After");
    port.current = event("event-1", "External");
    CalendarHistoryExecutor executor{port};

    const auto result = QCoro::waitFor(
        executor.execute(entry(before, after, after.id), HistoryExecutionDirection::Undo));
    CHECK(result.outcome == HistoryExecutionOutcome::Conflict);
    CHECK(port.mutations == 0);
    CHECK(port.current->title == "External");
}
