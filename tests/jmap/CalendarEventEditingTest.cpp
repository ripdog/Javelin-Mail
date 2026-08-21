#include "jmap/calendar/CalendarEventEditing.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using javelin::jmap::calendar::Attendee;

namespace
{
    Attendee attendee(std::string id, std::string address, const bool owner = false)
    {
        return {.id = std::move(id),
                .name = address,
                .email = address,
                .calendarAddress = "mailto:" + address,
                .participationStatus = owner ? "accepted" : "tentative",
                .isOwner = owner,
                .isAttendee = true,
                .roles = {},
                .expectReply = !owner,
                .scheduleSequence = 7,
                .scheduleUpdated = std::nullopt};
    }
} // namespace

TEST_CASE("editable attendee addresses exclude the owner")
{
    const std::vector attendees{attendee("owner", "owner@example.test", true),
                                attendee("guest", "guest@example.test")};

    CHECK(javelin::jmap::calendar::editableAttendeeAddresses(attendees) ==
          std::vector<std::string>{"guest@example.test"});
}

TEST_CASE("calendar ownership matches the configured identity independently of isOrigin")
{
    javelin::jmap::calendar::CalendarEvent event;
    event.isOrigin = false;
    event.attendees = {attendee("owner", "Owner@Example.test", true),
                       attendee("guest", "guest@example.test")};

    CHECK(javelin::jmap::calendar::eventOwnedByAddress(event, "owner@example.test"));
    CHECK(javelin::jmap::calendar::eventOwnedByAddress(event, "MAILTO:OWNER@EXAMPLE.TEST"));
    CHECK_FALSE(javelin::jmap::calendar::eventOwnedByAddress(event, "guest@example.test"));
    CHECK_FALSE(javelin::jmap::calendar::eventOwnedByAddress(event, "missing@example.test"));
}

TEST_CASE("calendar participant matching accepts the participant email fallback")
{
    javelin::jmap::calendar::CalendarEvent event;
    auto participant = attendee("owner", "owner@example.test", true);
    participant.calendarAddress = "urn:uuid:calendar-owner";
    event.attendees = {participant};

    CHECK(javelin::jmap::calendar::participantIndexForAddress(event, "owner@example.test") == 0);
}

TEST_CASE("calendar ownership falls back to the organizer address")
{
    javelin::jmap::calendar::CalendarEvent event;
    event.isOrigin = false;
    event.organizerCalendarAddress = "mailto:owner@example.test";

    CHECK(javelin::jmap::calendar::eventOwnedByAddress(event, "OWNER@example.test"));
    CHECK_FALSE(javelin::jmap::calendar::eventOwnedByAddress(event, "guest@example.test"));
}

TEST_CASE("calendar owner and invitation classification are independent of isOrigin")
{
    javelin::jmap::calendar::CalendarEvent event;
    event.isOrigin = false;
    event.attendees = {attendee("owner", "owner@example.test", true),
                       attendee("guest", "guest@example.test")};

    CHECK(javelin::jmap::calendar::eventHasOwner(event));
    CHECK_FALSE(javelin::jmap::calendar::eventInvitesAddress(event, "owner@example.test"));
    CHECK(javelin::jmap::calendar::eventInvitesAddress(event, "guest@example.test"));
    event.attendees.clear();
    CHECK_FALSE(javelin::jmap::calendar::eventHasOwner(event));
}

TEST_CASE("calendar editability follows write-all write-own and invitation rights")
{
    javelin::jmap::calendar::CalendarEvent event;
    event.isOrigin = false;
    const javelin::jmap::calendar::CalendarRights writeOwn{.mayWriteOwn = true};
    const javelin::jmap::calendar::CalendarRights writeAll{.mayWriteAll = true};
    const javelin::jmap::calendar::CalendarRights readOnly;

    CHECK(javelin::jmap::calendar::eventEditableWithRights(event, writeOwn, "alice@example.test"));
    CHECK_FALSE(
        javelin::jmap::calendar::eventEditableWithRights(event, readOnly, "alice@example.test"));

    event.attendees = {attendee("owner", "owner@example.test", true),
                       attendee("me", "alice@example.test")};
    CHECK_FALSE(
        javelin::jmap::calendar::eventEditableWithRights(event, writeAll, "alice@example.test"));
    CHECK(javelin::jmap::calendar::eventEditableWithRights(event, writeAll, "other@example.test"));
    CHECK_FALSE(
        javelin::jmap::calendar::eventEditableWithRights(event, writeOwn, "other@example.test"));
}

TEST_CASE("attendee edits preserve matching scheduling records and owners")
{
    const std::vector existing{attendee("owner", "owner@example.test", true),
                               attendee("guest-id", "Guest@Example.test")};
    const std::vector<std::string> requested{"guest@example.test", "new@example.test",
                                             "new@example.test"};

    const auto result = javelin::jmap::calendar::reconcileEditableAttendees(existing, requested);

    REQUIRE(result.size() == 3);
    CHECK(result[0].id == "owner");
    CHECK(result[0].isOwner);
    CHECK(result[1].id == "guest-id");
    CHECK(result[1].participationStatus == "tentative");
    CHECK(result[1].expectReply);
    CHECK(result[1].scheduleSequence == 7);
    CHECK(result[2].id == "attendee-1");
    CHECK(result[2].email == std::optional<std::string>{"new@example.test"});
    CHECK(result[2].participationStatus == "needs-action");
    CHECK(result[2].expectReply);
}

TEST_CASE("attendee suggestions split display names from email addresses")
{
    const std::vector<javelin::jmap::calendar::Attendee> existing;
    const std::vector<std::string> requested{"Carol Person <carol@example.test>"};

    const auto result = javelin::jmap::calendar::reconcileEditableAttendees(existing, requested);

    REQUIRE(result.size() == 1);
    CHECK(result.front().name == "Carol Person");
    CHECK(result.front().email == std::optional<std::string>{"carol@example.test"});
    CHECK(result.front().calendarAddress == "mailto:carol@example.test");
}

TEST_CASE("removed editable attendees do not remove hidden participant records")
{
    auto hidden = attendee("resource", "room@example.test");
    hidden.isAttendee = false;
    const std::vector existing{hidden, attendee("guest", "guest@example.test")};
    const std::vector<std::string> requested;

    const auto result = javelin::jmap::calendar::reconcileEditableAttendees(existing, requested);

    REQUIRE(result.size() == 1);
    CHECK(result.front().id == "resource");
}

TEST_CASE("effective recurring occurrences apply participant overrides")
{
    javelin::jmap::calendar::CalendarEvent base;
    base.id = "series";
    base.start = {.value = "2026-09-01T09:00:00"};
    base.duration = {.value = "PT1H"};
    base.recurrenceRule = javelin::jmap::calendar::RecurrenceRule{};
    base.attendees = {attendee("me", "alice@example.test")};
    base.attendees.front().participationStatus = "accepted";
    auto alias = attendee("alias", "alias@example.test");
    alias.participationStatus = "needs-action";
    auto& occurrence = base.recurrenceOverrides["2026-09-08T09:00:00"];
    occurrence.participantParticipationStatus.insert_or_assign("me", "needs-action");
    occurrence.participantOverrides.insert_or_assign("alias", alias);

    const auto effective =
        javelin::jmap::calendar::effectiveOccurrenceEvent(base, {.value = "2026-09-08T09:00:00"});

    REQUIRE(effective.has_value());
    CHECK(effective->recurrenceId ==
          javelin::jmap::calendar::LocalDateTime{.value = "2026-09-08T09:00:00"});
    REQUIRE(effective->attendees.size() == 2);
    CHECK(effective->attendees.front().participationStatus == "needs-action");
    CHECK(effective->attendees.back().id == "alias");
    CHECK(effective->attendees.back().participationStatus == "needs-action");
    CHECK_FALSE(effective->recurrenceRule.has_value());
    CHECK(effective->recurrenceOverrides.empty());
}

TEST_CASE("occurrence RSVP changes only the participant status override")
{
    javelin::jmap::calendar::CalendarEvent base;
    base.id = "series";
    base.attendees = {attendee("me", "alice@example.test")};
    base.attendees.front().participationStatus = "accepted";
    base.recurrenceRule = javelin::jmap::calendar::RecurrenceRule{};
    base.recurrenceOverrides["2026-09-08T09:00:00"].participantParticipationStatus.insert_or_assign(
        "me", "needs-action");

    const auto accepted = javelin::jmap::calendar::setOccurrenceParticipationStatus(
        base, {.value = "2026-09-08T09:00:00"}, "me", "accepted");
    CHECK_FALSE(accepted.recurrenceOverrides.contains("2026-09-08T09:00:00"));
    CHECK(accepted.attendees.front().participationStatus == "accepted");

    const auto declined = javelin::jmap::calendar::setOccurrenceParticipationStatus(
        base, {.value = "2026-09-08T09:00:00"}, "me", "declined");
    CHECK(declined.recurrenceOverrides.at("2026-09-08T09:00:00")
              .participantParticipationStatus.at("me") == "declined");
    CHECK(declined.attendees.front().participationStatus == "accepted");

    base.recurrenceOverrides["2026-09-15T09:00:00"].excluded = true;
    const auto excluded = javelin::jmap::calendar::setOccurrenceParticipationStatus(
        base, {.value = "2026-09-15T09:00:00"}, "me", "declined");
    REQUIRE(excluded.recurrenceOverrides.contains("2026-09-15T09:00:00"));
    CHECK(excluded.recurrenceOverrides.at("2026-09-15T09:00:00").excluded);
}

TEST_CASE("occurrence edits create an override without changing the base event")
{
    javelin::jmap::calendar::CalendarEvent base;
    base.id = "series";
    base.title = "Daily meeting";
    base.start = {.value = "2026-07-13T09:00:00"};
    base.duration = {.value = "PT1H"};
    base.recurrenceRule = javelin::jmap::calendar::RecurrenceRule{};
    base.recurrenceOverrides.emplace(
        "2026-07-14T09:00:00",
        javelin::jmap::calendar::RecurrenceOverride{.excluded = false,
                                                    .start = std::nullopt,
                                                    .duration = std::nullopt,
                                                    .title = "Old title",
                                                    .participantOverrides = {},
                                                    .participantParticipationStatus = {}});
    auto edited = base;
    edited.title = "Moved meeting";
    edited.start = {.value = "2026-07-14T10:30:00"};
    edited.duration = {.value = "PT30M"};

    const auto result = javelin::jmap::calendar::applyOccurrenceEdit(
        base, {.value = "2026-07-14T09:00:00"}, edited);

    CHECK(result.id == "series");
    CHECK(result.title == "Daily meeting");
    CHECK(result.start.value == "2026-07-13T09:00:00");
    REQUIRE(result.recurrenceRule.has_value());
    const auto& occurrence = result.recurrenceOverrides.at("2026-07-14T09:00:00");
    CHECK_FALSE(occurrence.excluded);
    CHECK(occurrence.start ==
          javelin::jmap::calendar::LocalDateTime{.value = "2026-07-14T10:30:00"});
    CHECK(occurrence.duration == javelin::jmap::calendar::Duration{.value = "PT30M"});
    CHECK(occurrence.title == std::optional<std::string>{"Moved meeting"});
}

TEST_CASE("deleting one occurrence preserves its existing override as an exclusion")
{
    javelin::jmap::calendar::CalendarEvent base;
    base.id = "series";
    base.recurrenceOverrides.emplace(
        "2026-07-14T09:00:00",
        javelin::jmap::calendar::RecurrenceOverride{
            .excluded = false,
            .start = javelin::jmap::calendar::LocalDateTime{.value = "2026-07-14T10:00:00"},
            .duration = std::nullopt,
            .title = "Changed",
            .participantOverrides = {},
            .participantParticipationStatus = {}});

    const auto result =
        javelin::jmap::calendar::excludeOccurrence(base, {.value = "2026-07-14T09:00:00"});

    const auto& occurrence = result.recurrenceOverrides.at("2026-07-14T09:00:00");
    CHECK(occurrence.excluded);
    CHECK(occurrence.start ==
          javelin::jmap::calendar::LocalDateTime{.value = "2026-07-14T10:00:00"});
    CHECK(occurrence.title == std::optional<std::string>{"Changed"});
}

TEST_CASE("occurrence edits only override properties that differ from the series")
{
    javelin::jmap::calendar::CalendarEvent base;
    base.id = "series";
    base.title = "Daily meeting";
    base.start = {.value = "2026-07-13T09:00:00"};
    base.duration = {.value = "PT1H"};
    auto edited = base;
    edited.title = "Special meeting";
    edited.start = {.value = "2026-07-14T09:00:00"};

    const auto result = javelin::jmap::calendar::applyOccurrenceEdit(
        base, {.value = "2026-07-14T09:00:00"}, edited);

    const auto& occurrence = result.recurrenceOverrides.at("2026-07-14T09:00:00");
    CHECK_FALSE(occurrence.start.has_value());
    CHECK_FALSE(occurrence.duration.has_value());
    CHECK(occurrence.title == std::optional<std::string>{"Special meeting"});
}

TEST_CASE("acknowledging a calendar alert materializes it on the base event")
{
    javelin::jmap::calendar::CalendarEvent event;
    event.id = "event-1";
    event.useDefaultAlerts = true;
    const javelin::jmap::calendar::Alert defaultAlert{
        .id = "default-10m",
        .action = "display",
        .triggerKind = javelin::jmap::calendar::AlertTriggerKind::Offset,
        .relativeTo = "start",
        .offset = javelin::jmap::calendar::Duration{.value = "-PT10M"},
        .when = std::nullopt,
        .acknowledged = std::nullopt};

    const auto acknowledged = javelin::jmap::calendar::acknowledgeAlert(
        event, defaultAlert, {.value = "2026-07-28T04:05:06.000Z"});

    REQUIRE(acknowledged.alerts.contains("default-10m"));
    const auto& alert = acknowledged.alerts.at("default-10m");
    CHECK(alert.offset == javelin::jmap::calendar::Duration{.value = "-PT10M"});
    CHECK(alert.acknowledged ==
          javelin::jmap::calendar::UtcInstant{.value = "2026-07-28T04:05:06.000Z"});
}

TEST_CASE("acknowledging a removed calendar alert does not restore it")
{
    javelin::jmap::calendar::CalendarEvent event;
    event.id = "event-1";
    event.useDefaultAlerts = false;
    const javelin::jmap::calendar::Alert removedAlert{
        .id = "removed-10m",
        .action = "display",
        .triggerKind = javelin::jmap::calendar::AlertTriggerKind::Offset,
        .relativeTo = "start",
        .offset = javelin::jmap::calendar::Duration{.value = "-PT10M"},
        .when = std::nullopt,
        .acknowledged = std::nullopt};

    const auto acknowledged = javelin::jmap::calendar::acknowledgeAlert(
        event, removedAlert, {.value = "2026-07-28T04:05:06.000Z"});

    CHECK(acknowledged == event);
    CHECK_FALSE(acknowledged.alerts.contains("removed-10m"));
}
