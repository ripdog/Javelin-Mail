#include "jmap/api/CalendarMethods.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("calendar writable comparison ignores server-derived fields", "[jmap][calendar]")
{
    javelin::jmap::calendar::CalendarEvent submitted;
    submitted.accountId = "a1";
    submitted.uid = "uid-1";
    submitted.calendarIds = {{"calendar-1", true}};
    submitted.title = "Appointment";
    submitted.start = {.value = "2026-08-01T09:00:00"};
    submitted.duration = {.value = "PT1H"};
    submitted.timeZone = javelin::jmap::calendar::TimeZoneId{.value = "Pacific/Auckland"};

    auto accepted = submitted;
    accepted.id = "event-1";
    accepted.baseEventId = "server-base";
    accepted.recurrenceId = javelin::jmap::calendar::LocalDateTime{.value = "2026-08-01T09:00:00"};
    accepted.isOrigin = true;
    accepted.utcStart = javelin::jmap::calendar::UtcInstant{.value = "2026-07-31T21:00:00Z"};
    accepted.utcEnd = javelin::jmap::calendar::UtcInstant{.value = "2026-07-31T22:00:00Z"};

    CHECK(javelin::jmap::api::calendarEventWritablePropertiesEqual(submitted, accepted));
    accepted.title = "Changed elsewhere";
    CHECK_FALSE(javelin::jmap::api::calendarEventWritablePropertiesEqual(submitted, accepted));
}

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

TEST_CASE("calendar event get serializes result-referenced query ids", "[jmap][calendar]")
{
    const javelin::jmap::api::CallHandle<javelin::jmap::api::CalendarEventQueryResponse>
        queryHandle{.callId = "expanded-query"};
    const auto request = javelin::jmap::api::calendarEventGet({
        .accountId = "a1",
        .ids = std::nullopt,
        .idsReference = javelin::jmap::api::resultReference(queryHandle, "/ids"),
        .properties = std::nullopt,
        .recurrenceOverridesBefore = std::nullopt,
        .recurrenceOverridesAfter = std::nullopt,
        .reduceParticipants = false,
        .timeZone = {.value = "Pacific/Auckland"},
    });

    REQUIRE(request.has_value());
    CHECK(
        request->arguments.find(
            R"("#ids":{"resultOf":"expanded-query","name":"CalendarEvent/query","path":"/ids"})") !=
        std::string::npos);
    CHECK(request->arguments.find(R"("ids")") == std::string::npos);
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
    const auto method = javelin::jmap::api::calendarSet({.accountId = "a1",
                                                         .ifInState = "calendar-state-1",
                                                         .create = {},
                                                         .destroy = {},
                                                         .onDestroyRemoveEvents = false,
                                                         .onSuccessSetIsDefault = "personal"});
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

TEST_CASE("calendar set serializes creation and destructive deletion", "[jmap][calendar]")
{
    javelin::jmap::calendar::Calendar calendar{
        .accountId = "a1",
        .id = {},
        .name = "Projects",
        .description = std::nullopt,
        .color = "#336699",
        .sortOrder = 0,
        .isSubscribed = true,
        .isVisible = true,
        .isDefault = false,
        .timeZone = std::nullopt,
        .defaultAlertsWithTime = {},
        .defaultAlertsWithoutTime = {},
        .myRights = {},
    };
    const auto method = javelin::jmap::api::calendarSet({
        .accountId = "a1",
        .ifInState = "c1",
        .create = {{"new-calendar", calendar}},
        .destroy = {"old-calendar"},
        .onDestroyRemoveEvents = true,
        .onSuccessSetIsDefault = std::nullopt,
    });
    REQUIRE(method.has_value());
    CHECK(method->arguments.find(R"("new-calendar":{"name":"Projects")") != std::string::npos);
    CHECK(method->arguments.find(R"("color":"#336699")") != std::string::npos);
    CHECK(method->arguments.find(R"("destroy":["old-calendar"])") != std::string::npos);
    CHECK(method->arguments.find(R"("onDestroyRemoveEvents":true)") != std::string::npos);
    CHECK(method->arguments.find(R"("myRights")") == std::string::npos);
    CHECK(method->arguments.find(R"("isDefault")") == std::string::npos);

    const auto response = javelin::jmap::api::parseCalendarSetResponse(
        R"({"accountId":"a1","oldState":"c1","newState":"c2","created":{"new-calendar":{"id":"calendar-2","isDefault":false}},"updated":{},"destroyed":["old-calendar"],"notCreated":{},"notUpdated":{},"notDestroyed":{}})");
    REQUIRE(response.ok());
    CHECK(response.value->created.at("new-calendar").id ==
          std::optional<std::string>{"calendar-2"});
    CHECK(response.value->destroyed == std::vector<std::string>{"old-calendar"});
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

TEST_CASE("calendar event documents round-trip complete custom recurrence rules",
          "[jmap][calendar][recurrence]")
{
    const auto parsed = javelin::jmap::api::parseCalendarEventGetResponse(
        R"({"accountId":"a1","state":"e5","list":[{"@type":"Event","id":"e1","uid":"uid-1","calendarIds":{"work":true},"title":"Complex","start":"2026-03-03T09:00:00","duration":"PT1H","timeZone":"Pacific/Auckland","showWithoutTime":false,"isDraft":false,"isOrigin":true,"recurrenceRule":{"@type":"RecurrenceRule","frequency":"yearly","interval":2,"rscale":"gregorian","skip":"backward","firstDayOfWeek":"su","byDay":[{"@type":"NDay","day":"mo"},{"@type":"NDay","day":"fr","nthOfPeriod":-1}],"byMonthDay":[1,-1],"byMonth":["3","6L"],"byYearDay":[100,-1],"byWeekNo":[1,-1],"byHour":[8,17],"byMinute":[0,30],"bySecond":[0,60],"bySetPosition":[1,-1],"count":9}}],"notFound":[]})");

    REQUIRE(parsed.ok());
    REQUIRE(parsed.value->list.size() == 1);
    const auto& event = parsed.value->list.front();
    REQUIRE(event.recurrenceRule.has_value());
    const auto& rule = *event.recurrenceRule;
    CHECK(rule.frequency == javelin::jmap::calendar::RecurrenceFrequency::Yearly);
    CHECK(rule.interval == 2);
    CHECK(rule.rscale == std::optional<std::string>{"gregorian"});
    CHECK(rule.skip == std::optional{javelin::jmap::calendar::RecurrenceSkip::Backward});
    CHECK(rule.firstDayOfWeek == std::optional{javelin::jmap::calendar::Weekday::Sunday});
    CHECK(rule.byDay ==
          std::vector{
              javelin::jmap::calendar::RecurrenceDay{
                  .day = javelin::jmap::calendar::Weekday::Monday, .nthOfPeriod = std::nullopt},
              javelin::jmap::calendar::RecurrenceDay{
                  .day = javelin::jmap::calendar::Weekday::Friday, .nthOfPeriod = -1}});
    CHECK(rule.byMonthDay == std::vector<std::int32_t>{1, -1});
    CHECK(rule.byMonth == std::vector<std::string>{"3", "6L"});
    CHECK(rule.byYearDay == std::vector<std::int32_t>{100, -1});
    CHECK(rule.byWeekNo == std::vector<std::int32_t>{1, -1});
    CHECK(rule.byHour == std::vector<std::uint32_t>{8, 17});
    CHECK(rule.byMinute == std::vector<std::uint32_t>{0, 30});
    CHECK(rule.bySecond == std::vector<std::uint32_t>{0, 60});
    CHECK(rule.bySetPosition == std::vector<std::int32_t>{1, -1});
    CHECK(rule.count == std::optional<std::uint32_t>{9});

    auto previous = event;
    previous.recurrenceRule = std::nullopt;
    const auto method = javelin::jmap::api::calendarEventSet(
        {.accountId = "a1",
         .ifInState = "e5",
         .create = {},
         .update = {{"e1", {.previous = previous, .event = event}}},
         .destroy = {},
         .sendSchedulingMessages = true});
    REQUIRE(method.has_value());
    CHECK(method->arguments.find(R"("rscale":"gregorian")") != std::string::npos);
    CHECK(method->arguments.find(R"("skip":"backward")") != std::string::npos);
    CHECK(method->arguments.find(R"("firstDayOfWeek":"su")") != std::string::npos);
    CHECK(method->arguments.find(R"("nthOfPeriod":-1)") != std::string::npos);
    CHECK(method->arguments.find(R"("byMonthDay":[1,-1])") != std::string::npos);
    CHECK(method->arguments.find(R"("byMonth":["3","6L"])") != std::string::npos);
    CHECK(method->arguments.find(R"("bySetPosition":[1,-1])") != std::string::npos);
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
    const auto method = javelin::jmap::api::calendarEventSet(
        {.accountId = "a1",
         .ifInState = std::nullopt,
         .create = {},
         .update = {{"e1", {.previous = event, .event = event}}},
         .destroy = {},
         .sendSchedulingMessages = true});

    REQUIRE(method.has_value());
    CHECK(method->arguments.find(R"("update":{"e1":{}})") != std::string::npos);
    CHECK(method->arguments.find(R"("id":"e1")") == std::string::npos);
    CHECK(method->arguments.find(R"("uid")") == std::string::npos);
    CHECK(method->arguments.find(R"("isOrigin")") == std::string::npos);
    CHECK(method->arguments.find(R"("utcStart")") == std::string::npos);
    CHECK(method->arguments.find(R"("utcEnd")") == std::string::npos);
}

TEST_CASE("calendar alert acknowledgements use exact patch paths", "[jmap][calendar][alerts]")
{
    javelin::jmap::calendar::CalendarEvent previous{
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
        .alerts = {{"alert-1",
                    {.id = "alert-1",
                     .action = "display",
                     .triggerKind = javelin::jmap::calendar::AlertTriggerKind::Offset,
                     .relativeTo = "start",
                     .offset = javelin::jmap::calendar::Duration{.value = "-PT10M"},
                     .when = std::nullopt,
                     .acknowledged = std::nullopt}}},
        .utcStart = std::nullopt,
        .utcEnd = std::nullopt,
        .recurrenceRule = std::nullopt,
        .recurrenceOverrides = {},
        .attendees = {}};

    auto current = previous;
    current.alerts.at("alert-1").acknowledged =
        javelin::jmap::calendar::UtcInstant{.value = "2026-03-03T08:50:00.000Z"};
    const auto method = javelin::jmap::api::calendarEventSet(
        {.accountId = "a1",
         .ifInState = "event-state-1",
         .create = {},
         .update = {{"e1", {.previous = previous, .event = current}}},
         .destroy = {},
         .sendSchedulingMessages = true});

    REQUIRE(method.has_value());
    CHECK(method->arguments.find(R"("alerts/alert-1/acknowledged":")") != std::string::npos);
    CHECK(method->arguments.find(R"("alerts":{"alert-1")") == std::string::npos);
    CHECK(method->arguments.find(R"("uid")") == std::string::npos);

    auto defaultPrevious = previous;
    defaultPrevious.useDefaultAlerts = true;
    defaultPrevious.alerts.clear();
    auto defaultCurrent = defaultPrevious;
    defaultCurrent.alerts.emplace(
        "default-alert", javelin::jmap::calendar::Alert{
                             .id = "default-alert",
                             .action = "display",
                             .triggerKind = javelin::jmap::calendar::AlertTriggerKind::Offset,
                             .relativeTo = "start",
                             .offset = javelin::jmap::calendar::Duration{.value = "-PT10M"},
                             .when = std::nullopt,
                             .acknowledged = javelin::jmap::calendar::UtcInstant{
                                 .value = "2026-03-03T08:50:00.000Z"}});
    const auto defaultMethod = javelin::jmap::api::calendarEventSet(
        {.accountId = "a1",
         .ifInState = "event-state-1",
         .create = {},
         .update = {{"e1", {.previous = defaultPrevious, .event = defaultCurrent}}},
         .destroy = {},
         .sendSchedulingMessages = true});

    REQUIRE(defaultMethod.has_value());
    CHECK(defaultMethod->arguments.find(R"("alerts":{"default-alert")") != std::string::npos);
    CHECK(defaultMethod->arguments.find(R"("alerts/default-alert")") == std::string::npos);
    CHECK(defaultMethod->arguments.find(R"("uid")") == std::string::npos);
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
    series.recurrenceRule = javelin::jmap::calendar::RecurrenceRule{};
    series.recurrenceRule->frequency = javelin::jmap::calendar::RecurrenceFrequency::Weekly;
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

    auto previous = series;
    previous.recurrenceOverrides.clear();
    const auto method = javelin::jmap::api::calendarEventSet(
        {.accountId = "a1",
         .ifInState = "event-state-4",
         .create = {},
         .update = {{"base-series", {.previous = previous, .event = series}}},
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
