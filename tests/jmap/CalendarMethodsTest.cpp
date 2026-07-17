#include "jmap/api/CalendarMethods.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("calendar query serializes the bounded draft-26 recurrence shape", "[jmap][calendar]")
{
    const auto request = javelin::jmap::api::calendarEventQuery(
        {.accountId = "a1",
         .filter = {.inCalendar = std::nullopt,
                    .after = javelin::jmap::calendar::LocalDateTime{.value = "2026-03-01T00:00:00"},
                    .before =
                        javelin::jmap::calendar::LocalDateTime{.value = "2026-04-12T00:00:00"},
                    .text = std::nullopt},
         .expandRecurrences = true,
         .timeZone = {.value = "Pacific/Auckland"},
         .position = 0,
         .limit = 500,
         .calculateTotal = true});

    REQUIRE(request.has_value());
    CHECK(request->name == "CalendarEvent/query");
    CHECK(request->arguments.find(R"("expandRecurrences":true)") != std::string::npos);
    CHECK(request->arguments.find(R"("after":"2026-03-01T00:00:00")") != std::string::npos);
    CHECK(request->arguments.find(R"("before":"2026-04-12T00:00:00")") != std::string::npos);
    CHECK(request->arguments.find(R"("timeZone":"Pacific/Auckland")") != std::string::npos);
}

TEST_CASE("calendar query serializes an unbounded UID lookup", "[jmap][calendar]")
{
    const auto request =
        javelin::jmap::api::calendarEventQuery({.accountId = "a1",
                                                .filter = {.uid = "series-uid"},
                                                .expandRecurrences = false,
                                                .timeZone = {.value = "Pacific/Auckland"},
                                                .position = 0,
                                                .limit = std::nullopt,
                                                .calculateTotal = true});

    REQUIRE(request.has_value());
    CHECK(request->arguments.find(R"("uid":"series-uid")") != std::string::npos);
    CHECK(request->arguments.find(R"("expandRecurrences":false)") != std::string::npos);
    CHECK(request->arguments.find(R"("after")") == std::string::npos);
    CHECK(request->arguments.find(R"("before")") == std::string::npos);
}

TEST_CASE("calendar get parses draft-26 rights", "[jmap][calendar]")
{
    const auto parsed = javelin::jmap::api::parseCalendarGetResponse(
        R"({"accountId":"a1","state":"c2","list":[{"id":"work","name":"Work","color":"#2457a6","sortOrder":4,"isSubscribed":true,"isVisible":true,"isDefault":true,"myRights":{"mayReadFreeBusy":true,"mayReadItems":true,"mayWriteAll":true,"mayWriteOwn":true,"mayUpdatePrivate":true,"mayRSVP":true,"mayShare":false,"mayDelete":false}}],"notFound":[]})");

    REQUIRE(parsed.ok());
    REQUIRE(parsed.value->list.size() == 1);
    CHECK(parsed.value->list.front().accountId == "a1");
    CHECK(parsed.value->list.front().name == "Work");
    CHECK(parsed.value->list.front().myRights.mayReadItems);
    CHECK(parsed.value->list.front().myRights.mayWriteAll);
    CHECK_FALSE(parsed.value->list.front().myRights.mayDelete);
}

TEST_CASE("calendar set changes the server default through the draft argument", "[jmap][calendar]")
{
    const auto method = javelin::jmap::api::calendarSet(
        {.accountId = "a1", .ifInState = "calendar-state-1", .onSuccessSetIsDefault = "personal"});
    REQUIRE(method.has_value());
    CHECK(method->name == "Calendar/set");
    CHECK(method->arguments.find(R"("onSuccessSetIsDefault":"personal")") != std::string::npos);
    CHECK(method->arguments.find(R"("isDefault")") == std::string::npos);

    const auto response = javelin::jmap::api::parseCalendarSetResponse(
        R"({"accountId":"a1","oldState":"calendar-state-1","newState":"calendar-state-2","updated":{"work":{"isDefault":false},"personal":{"isDefault":true}},"notUpdated":{}})");
    REQUIRE(response.ok());
    CHECK(response.value->updated.at("work").isDefault == std::optional<bool>{false});
    CHECK(response.value->updated.at("personal").isDefault == std::optional<bool>{true});
}

TEST_CASE("calendar event documents preserve recurrence and attendees", "[jmap][calendar]")
{
    const auto parsed = javelin::jmap::api::parseCalendarEventGetResponse(
        R"({"accountId":"a1","state":"e4","list":[{"@type":"Event","id":"e1","uid":"uid-1","calendarIds":{"work":true},"title":"Planning","start":"2026-03-03T09:00:00","duration":"PT1H","timeZone":"Pacific/Auckland","showWithoutTime":false,"isDraft":false,"isOrigin":true,"recurrenceRule":{"@type":"RecurrenceRule","frequency":"weekly","interval":1},"recurrenceOverrides":{"2026-03-10T09:00:00":{"excluded":true}},"participants":{"p1":{"@type":"Participant","name":"Alice","email":"alice@example.test","calendarAddress":"mailto:alice@example.test","participationStatus":"accepted","roles":{"owner":true,"attendee":true},"scheduleSequence":2}}}],"notFound":[]})");

    REQUIRE(parsed.ok());
    REQUIRE(parsed.value->list.size() == 1);
    const auto& event = parsed.value->list.front();
    REQUIRE(event.recurrenceRule.has_value());
    CHECK(event.recurrenceRule->frequency == javelin::jmap::calendar::RecurrenceFrequency::Weekly);
    REQUIRE(event.recurrenceOverrides.contains("2026-03-10T09:00:00"));
    CHECK(event.recurrenceOverrides.at("2026-03-10T09:00:00").excluded);
    REQUIRE(event.attendees.size() == 1);
    CHECK(event.attendees.front().isOwner);
}

TEST_CASE("calendar recurrence parsing marks selector rules unsafe for simple expansion",
          "[jmap][calendar][recurrence]")
{
    const auto parsed = javelin::jmap::api::parseCalendarEventGetResponse(
        R"({"accountId":"a1","state":"e5","list":[{"@type":"Event","id":"e1","uid":"uid-1","calendarIds":{"work":true},"title":"Weekdays","start":"2026-03-03T09:00:00","duration":"PT1H","timeZone":"Pacific/Auckland","showWithoutTime":false,"isDraft":false,"isOrigin":true,"recurrenceRule":{"@type":"RecurrenceRule","frequency":"weekly","byDay":[{"@type":"NDay","day":"mo"},{"@type":"NDay","day":"we"}]}}],"notFound":[]})");

    REQUIRE(parsed.ok());
    REQUIRE(parsed.value->list.size() == 1);
    REQUIRE(parsed.value->list.front().recurrenceRule.has_value());
    CHECK(parsed.value->list.front().recurrenceRule->hasUnsupportedExpansionProperties);

    const auto simple = javelin::jmap::api::parseCalendarEventGetResponse(
        R"({"accountId":"a1","state":"e6","list":[{"@type":"Event","id":"e2","uid":"uid-2","calendarIds":{"work":true},"title":"Every three days","start":"2026-01-08T02:20:00","duration":"PT2H10M","timeZone":"Pacific/Auckland","showWithoutTime":false,"isDraft":false,"isOrigin":true,"recurrenceRule":{"@type":"RecurrenceRule","frequency":"daily","interval":3,"firstDayOfWeek":"su"}}],"notFound":[]})");
    REQUIRE(simple.ok());
    REQUIRE(simple.value->list.front().recurrenceRule.has_value());
    CHECK_FALSE(simple.value->list.front().recurrenceRule->hasUnsupportedExpansionProperties);
}

TEST_CASE("calendar event parsing preserves expanded instance identity", "[jmap][calendar]")
{
    const auto parsed = javelin::jmap::api::parseCalendarEventGetResponse(
        R"({"accountId":"a1","state":"e4","list":[{"@type":"Event","id":"synthetic-1","baseEventId":"base-1","recurrenceId":"2026-03-03T09:00:00","uid":"uid-1","calendarIds":{"work":true},"title":"Planning","start":"2026-03-03T09:00:00","duration":"PT1H","timeZone":"Pacific/Auckland","showWithoutTime":false,"isDraft":false,"isOrigin":true}],"notFound":[]})");

    REQUIRE(parsed.ok());
    REQUIRE(parsed.value->list.size() == 1);
    const auto& event = parsed.value->list.front();
    REQUIRE(event.baseEventId.has_value());
    CHECK(*event.baseEventId == "base-1");
    REQUIRE(event.recurrenceId.has_value());
    CHECK(event.recurrenceId->value == "2026-03-03T09:00:00");
}

TEST_CASE("calendar event parsing normalizes imported midnight date spans as all-day",
          "[jmap][calendar][stalwart]")
{
    const auto parsed = javelin::jmap::api::parseCalendarEventGetResponse(
        R"({"accountId":"c","state":"e4","list":[{"@type":"Event","id":"holiday","uid":"holiday-1","calendarIds":{"holidays":true},"title":"Matariki","start":"2026-07-10T00:00:00","duration":"P1D","timeZone":"Pacific/Auckland","showWithoutTime":false,"isDraft":false,"isOrigin":true}],"notFound":[]})");

    REQUIRE(parsed.ok());
    REQUIRE(parsed.value->list.size() == 1);
    CHECK(parsed.value->list.front().showWithoutTime);
}

TEST_CASE("calendar event parsing keeps midnight timed events timed", "[jmap][calendar]")
{
    const auto parsed = javelin::jmap::api::parseCalendarEventGetResponse(
        R"({"accountId":"c","state":"e4","list":[{"@type":"Event","id":"night","uid":"night-1","calendarIds":{"work":true},"title":"Midnight deployment","start":"2026-07-10T00:00:00","duration":"PT1H","timeZone":"Pacific/Auckland","showWithoutTime":false,"isDraft":false,"isOrigin":true}],"notFound":[]})");

    REQUIRE(parsed.ok());
    REQUIRE(parsed.value->list.size() == 1);
    CHECK_FALSE(parsed.value->list.front().showWithoutTime);
}

TEST_CASE("calendar event parsing preserves a floating null time zone", "[jmap][calendar]")
{
    const auto parsed = javelin::jmap::api::parseCalendarEventGetResponse(
        R"({"accountId":"c","state":"e4","list":[{"@type":"Event","id":"floating","uid":"floating-1","calendarIds":{"personal":true},"title":"Floating event","start":"2026-07-13T13:00:00","duration":"PT1H","timeZone":null,"showWithoutTime":false,"isDraft":false,"isOrigin":true}],"notFound":[]})");

    REQUIRE(parsed.ok());
    REQUIRE(parsed.value->list.size() == 1);
    CHECK_FALSE(parsed.value->list.front().timeZone.has_value());
}

TEST_CASE("calendar set exposes scheduling failures as typed errors", "[jmap][calendar]")
{
    const auto parsed = javelin::jmap::api::parseCalendarEventSetResponse(
        R"({"accountId":"a1","oldState":"e1","newState":"e1","created":{},"updated":{},"destroyed":[],"notCreated":{"new":{"type":"noSupportedScheduleMethods","description":"recipient has no supported method"}},"notUpdated":{},"notDestroyed":{}})");

    REQUIRE(parsed.ok());
    REQUIRE(parsed.value->notCreated.contains("new"));
    CHECK(parsed.value->notCreated.at("new").type ==
          javelin::jmap::api::CalendarSetErrorType::NoSupportedScheduleMethods);
}

TEST_CASE("calendar set accepts standard partial and null success results", "[jmap][calendar]")
{
    const auto parsed = javelin::jmap::api::parseCalendarEventSetResponse(
        R"({"accountId":"a1","oldState":"e1","newState":"e2","created":{"new":{"id":"e9"}},"updated":{"e1":null},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{}})");

    REQUIRE(parsed.ok());
    REQUIRE(parsed.value->created.at("new").id.has_value());
    CHECK(*parsed.value->created.at("new").id == "e9");
    REQUIRE(parsed.value->updated.contains("e1"));
    CHECK_FALSE(parsed.value->updated.at("e1").has_value());
}

TEST_CASE("calendar set omits server-set event properties", "[jmap][calendar]")
{
    const javelin::jmap::calendar::CalendarEvent event{
        .accountId = "a1",
        .id = "e1",
        .baseEventId = std::nullopt,
        .recurrenceId = std::nullopt,
        .uid = "uid-1",
        .calendarIds = {{"work", true}},
        .title = "Planning",
        .description = std::nullopt,
        .location = std::nullopt,
        .start = {.value = "2026-03-03T09:00:00"},
        .duration = {.value = "PT1H"},
        .timeZone = javelin::jmap::calendar::TimeZoneId{.value = "Pacific/Auckland"},
        .showWithoutTime = false,
        .isDraft = false,
        .isOrigin = true,
        .useDefaultAlerts = false,
        .alerts = {},
        .utcStart = javelin::jmap::calendar::UtcInstant{.value = "2026-03-02T20:00:00Z"},
        .utcEnd = javelin::jmap::calendar::UtcInstant{.value = "2026-03-02T21:00:00Z"},
        .recurrenceRule = std::nullopt,
        .recurrenceOverrides = {},
        .attendees = {}};
    const auto method = javelin::jmap::api::calendarEventSet({.accountId = "a1",
                                                              .ifInState = std::nullopt,
                                                              .create = {},
                                                              .update = {{"e1", event}},
                                                              .destroy = {},
                                                              .sendSchedulingMessages = true});

    REQUIRE(method.has_value());
    CHECK(method->arguments.find(R"("id":"e1")") == std::string::npos);
    CHECK(method->arguments.find(R"("uid":"uid-1")") == std::string::npos);
    CHECK(method->arguments.find(R"("isOrigin")") == std::string::npos);
    CHECK(method->arguments.find(R"("utcStart")") == std::string::npos);
    CHECK(method->arguments.find(R"("utcEnd")") == std::string::npos);
}

TEST_CASE("calendar set serializes occurrence edits as base-series overrides",
          "[jmap][calendar][recurrence]")
{
    javelin::jmap::calendar::CalendarEvent series;
    series.accountId = "a1";
    series.id = "base-series";
    series.uid = "uid-series";
    series.calendarIds = {{"work", true}};
    series.title = "Planning";
    series.start = {.value = "2026-07-13T09:00:00"};
    series.duration = {.value = "PT1H"};
    series.timeZone = javelin::jmap::calendar::TimeZoneId{.value = "Pacific/Auckland"};
    series.recurrenceRule = javelin::jmap::calendar::RecurrenceRule{
        .frequency = javelin::jmap::calendar::RecurrenceFrequency::Weekly,
        .interval = 1,
        .count = std::nullopt,
        .until = std::nullopt};
    series.recurrenceOverrides.emplace(
        "2026-07-20T09:00:00",
        javelin::jmap::calendar::RecurrenceOverride{
            .excluded = false,
            .start = javelin::jmap::calendar::LocalDateTime{.value = "2026-07-20T10:30:00"},
            .duration = javelin::jmap::calendar::Duration{.value = "PT30M"},
            .title = "Moved planning"});
    series.recurrenceOverrides.emplace(
        "2026-07-27T09:00:00", javelin::jmap::calendar::RecurrenceOverride{.excluded = true,
                                                                           .start = std::nullopt,
                                                                           .duration = std::nullopt,
                                                                           .title = std::nullopt});

    const auto method = javelin::jmap::api::calendarEventSet({.accountId = "a1",
                                                              .ifInState = "event-state-4",
                                                              .create = {},
                                                              .update = {{"base-series", series}},
                                                              .destroy = {},
                                                              .sendSchedulingMessages = true});

    REQUIRE(method.has_value());
    CHECK(method->arguments.find(R"("update":{"base-series":)") != std::string::npos);
    CHECK(method->arguments.find(R"("2026-07-20T09:00:00")") != std::string::npos);
    CHECK(method->arguments.find(R"("start":"2026-07-20T10:30:00")") != std::string::npos);
    CHECK(method->arguments.find(R"("duration":"PT30M")") != std::string::npos);
    CHECK(method->arguments.find(R"("title":"Moved planning")") != std::string::npos);
    CHECK(method->arguments.find(R"("2026-07-27T09:00:00":{"excluded":true)") != std::string::npos);
    CHECK(method->arguments.find("synthetic") == std::string::npos);
}
