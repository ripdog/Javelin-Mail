#include "jmap/api/CalendarMethods.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("calendar query serializes the bounded draft-26 recurrence shape", "[jmap][calendar]")
{
    const auto request = javelin::jmap::api::calendarEventQuery(
        {.accountId = "a1",
         .filter = {.inCalendar = std::nullopt,
                    .after = {.value = "2026-03-01T00:00:00"},
                    .before = {.value = "2026-04-12T00:00:00"},
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
        .uid = "uid-1",
        .calendarIds = {{"work", true}},
        .title = "Planning",
        .description = std::nullopt,
        .location = std::nullopt,
        .start = {.value = "2026-03-03T09:00:00"},
        .duration = {.value = "PT1H"},
        .timeZone = {.value = "Pacific/Auckland"},
        .showWithoutTime = false,
        .isDraft = false,
        .isOrigin = true,
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
