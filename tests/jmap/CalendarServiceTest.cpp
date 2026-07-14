#include "jmap/calendar/CalendarService.h"

#include "jmap/api/JmapMethodTransport.h"
#include "jmap/cache/SessionRepository.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

namespace
{
    class FakeMethodTransport final : public javelin::jmap::api::JmapMethodTransport
    {
      public:
        std::optional<javelin::jmap::api::JmapMethodRequest> request;
        std::vector<javelin::jmap::api::JmapMethodTransportResult> results;

        QCoro::Task<javelin::jmap::api::JmapMethodTransportResult>
        call(javelin::jmap::api::JmapMethodRequest value) override
        {
            request = std::move(value);
            REQUIRE_FALSE(results.empty());
            auto result = std::move(results.front());
            results.erase(results.begin());
            co_return result;
        }
    };

    void ensureApplication()
    {
        if (QCoreApplication::instance() != nullptr)
            return;
        static int argc = 1;
        static char name[] = "calendar-service-test";
        static char* argv[] = {name, nullptr};
        static QCoreApplication application(argc, argv);
        Q_UNUSED(application);
    }

    javelin::jmap::api::Session session()
    {
        javelin::jmap::api::Session value;
        value.username = "alice@example.test";
        value.apiUrl = "https://example.test/jmap";
        value.downloadUrl = "https://example.test/download/{accountId}/{blobId}/{name}";
        value.uploadUrl = "https://example.test/upload/{accountId}";
        value.state = "session-1";
        value.capabilities.core = true;
        value.capabilities.calendars = true;
        value.primaryAccounts.calendarsAccountId = "a1";
        value.accounts.emplace(
            "a1", javelin::jmap::api::Account{
                      .id = "a1",
                      .name = "Calendar",
                      .isPersonal = true,
                      .isReadOnly = false,
                      .accountCapabilities = {.mail = false,
                                              .submission = false,
                                              .contacts = std::nullopt,
                                              .calendars = javelin::jmap::api::CalendarsCapability{
                                                  .maxCalendarsPerEvent = 4,
                                                  .minDateTime = "1900-01-01T00:00:00Z",
                                                  .maxDateTime = "2100-01-01T00:00:00Z",
                                                  .maxExpandedQueryDuration = "P1Y",
                                                  .maxParticipantsPerEvent = 100,
                                                  .mayCreateCalendar = false}}});
        return value;
    }

    javelin::jmap::calendar::CalendarEvent event()
    {
        javelin::jmap::calendar::CalendarEvent value;
        value.accountId = "a1";
        value.id = "event-1";
        value.uid = "uid-1";
        value.calendarIds = {{"work", true}};
        value.title = "Updated";
        value.start = {.value = "2026-07-13T09:00:00"};
        value.duration = {.value = "PT1H"};
        value.timeZone = javelin::jmap::calendar::TimeZoneId{.value = "Pacific/Auckland"};
        return value;
    }
} // namespace

TEST_CASE("calendar mutations use the cached event state", "[jmap][calendar][service]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-service-state"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    if (const auto error = sessions.replace("a1", session()))
        FAIL(error->message.toStdString());

    javelin::jmap::cache::CalendarRepository calendars{connection};
    REQUIRE_FALSE(calendars
                      .replaceCalendars(
                          "a1", "calendar-state-1",
                          {javelin::jmap::calendar::Calendar{.accountId = "a1",
                                                             .id = "work",
                                                             .name = "Work",
                                                             .description = std::nullopt,
                                                             .color = std::nullopt,
                                                             .sortOrder = 0,
                                                             .isSubscribed = true,
                                                             .isVisible = true,
                                                             .isDefault = true,
                                                             .timeZone = std::nullopt,
                                                             .myRights = {.mayReadFreeBusy = true,
                                                                          .mayReadItems = true,
                                                                          .mayWriteAll = true,
                                                                          .mayWriteOwn = true,
                                                                          .mayUpdatePrivate = true,
                                                                          .mayRSVP = true,
                                                                          .mayShare = false,
                                                                          .mayDelete = false}}})
                      .has_value());
    REQUIRE_FALSE(calendars
                      .reconcileWindow({.accountId = "a1",
                                        .start = {.value = "2026-06-29T00:00:00"},
                                        .end = {.value = "2026-08-10T00:00:00"},
                                        .displayTimeZone = {.value = "Pacific/Auckland"},
                                        .queryState = "query-state-1",
                                        .eventState = "event-state-7",
                                        .events = {},
                                        .occurrences = {}})
                      .has_value());

    FakeMethodTransport transport;
    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "CalendarEvent/set",
              .arguments =
                  R"({"accountId":"a1","oldState":"event-state-7","newState":"event-state-8","created":{},"updated":{"event-1":null},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{}})",
              .callId = "calendar-event-set"}},
        .createdIds = std::nullopt,
        .sessionState = "session-2"});
    javelin::jmap::calendar::CalendarService service{connection, transport};
    const auto result = QCoro::waitFor(
        service.update({.sessionUrl = "https://example.test/.well-known/jmap",
                        .loginEmail = "alice@example.test",
                        .apiKey = "secret"},
                       "a1", {.accountId = "a1", .event = event(), .ifInState = std::nullopt}));

    REQUIRE(std::holds_alternative<javelin::jmap::calendar::CommittedMutation>(result));
    REQUIRE(transport.request.has_value());
    REQUIRE(transport.request->envelope.methodCalls.size() == 1);
    CHECK(transport.request->envelope.methodCalls.front().arguments.find(
              R"("ifInState":"event-state-7")") != std::string::npos);

    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "Calendar/changes",
              .arguments =
                  R"({"accountId":"a1","oldState":"calendar-state-1","newState":"calendar-state-2","hasMoreChanges":false,"created":[],"updated":[],"destroyed":[]})",
              .callId = "calendar-changes"},
             {.name = "CalendarEvent/changes",
              .arguments =
                  R"({"accountId":"a1","oldState":"event-state-7","newState":"event-state-8","hasMoreChanges":false,"created":[],"updated":[],"destroyed":[]})",
              .callId = "calendar-event-changes"}},
        .createdIds = std::nullopt,
        .sessionState = "session-3"});
    const auto unchanged = QCoro::waitFor(service.refreshChanged(
        {.sessionUrl = "https://example.test/.well-known/jmap",
         .loginEmail = "alice@example.test",
         .apiKey = "secret"},
        "a1", {.start = {.value = "2026-06-29T00:00:00"}, .end = {.value = "2026-08-10T00:00:00"}},
        {.value = "Pacific/Auckland"}));

    REQUIRE(std::holds_alternative<javelin::jmap::calendar::RefreshedRange>(unchanged));
    CHECK(std::get<javelin::jmap::calendar::RefreshedRange>(unchanged).accountCount == 0);
    REQUIRE(transport.request.has_value());
    REQUIRE(transport.request->envelope.methodCalls.size() == 2);
    CHECK(transport.request->envelope.methodCalls[0].name == "Calendar/changes");
    CHECK(transport.request->envelope.methodCalls[1].name == "CalendarEvent/changes");
    const auto calendarState = calendars.stateToken("a1", "Calendar");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(calendarState));
    CHECK(std::get<std::optional<std::string>>(calendarState) ==
          std::optional<std::string>{"calendar-state-2"});
    const auto eventState = calendars.stateToken("a1", "CalendarEvent");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(eventState));
    CHECK(std::get<std::optional<std::string>>(eventState) ==
          std::optional<std::string>{"event-state-8"});

    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "Calendar/changes",
              .arguments =
                  R"({"accountId":"a1","oldState":"calendar-state-2","newState":"calendar-state-2","hasMoreChanges":false,"created":[],"updated":[],"destroyed":[]})",
              .callId = "calendar-changes"},
             {.name = "CalendarEvent/changes",
              .arguments =
                  R"({"accountId":"a1","oldState":"event-state-8","newState":"event-state-9","hasMoreChanges":false,"created":[],"updated":["event-1"],"destroyed":[]})",
              .callId = "calendar-event-changes"}},
        .createdIds = std::nullopt,
        .sessionState = "session-4"});
    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "CalendarEvent/get",
              .arguments =
                  R"({"accountId":"a1","state":"event-state-9","list":[{"@type":"Event","id":"event-1","uid":"uid-1","calendarIds":{"work":true},"title":"Changed remotely","start":"2026-07-13T10:00:00","duration":"PT1H","timeZone":"Pacific/Auckland","showWithoutTime":false,"isDraft":false,"isOrigin":true}],"notFound":[]})",
              .callId = "changed-calendar-events"}},
        .createdIds = std::nullopt,
        .sessionState = "session-5"});
    const auto changed = QCoro::waitFor(service.refreshChanged(
        {.sessionUrl = "https://example.test/.well-known/jmap",
         .loginEmail = "alice@example.test",
         .apiKey = "secret"},
        "a1", {.start = {.value = "2026-06-29T00:00:00"}, .end = {.value = "2026-08-10T00:00:00"}},
        {.value = "Pacific/Auckland"}));

    REQUIRE(std::holds_alternative<javelin::jmap::calendar::RefreshedRange>(changed));
    CHECK(std::get<javelin::jmap::calendar::RefreshedRange>(changed).accountCount == 1);
    REQUIRE(transport.request.has_value());
    REQUIRE(transport.request->envelope.methodCalls.size() == 1);
    CHECK(transport.request->envelope.methodCalls.front().name == "CalendarEvent/get");
    const auto changedWindow =
        calendars.loadWindow("a1", {.value = "2026-06-29T00:00:00"},
                             {.value = "2026-08-10T00:00:00"}, {.value = "Pacific/Auckland"});
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::CalendarWindow>>(changedWindow));
    const auto& cached =
        std::get<std::optional<javelin::jmap::cache::CalendarWindow>>(changedWindow);
    REQUIRE(cached.has_value());
    REQUIRE(cached->events.size() == 1);
    CHECK(cached->events.front().title == "Changed remotely");
}
