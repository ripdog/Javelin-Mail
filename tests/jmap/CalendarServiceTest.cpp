#include "jmap/calendar/CalendarService.h"

#include "jmap/api/JmapMethodTransport.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/calendar/CalendarMutationJournal.h"
#include "jmap/sync/ConsistencyDomain.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace
{
    class FakeMethodTransport final : public javelin::jmap::api::JmapMethodTransport
    {
      public:
        std::optional<javelin::jmap::api::JmapMethodRequest> request;
        std::vector<javelin::jmap::api::JmapMethodRequest> requests;
        std::vector<javelin::jmap::api::JmapMethodTransportResult> results;
        std::function<void()> beforeReturn;

        QCoro::Task<javelin::jmap::api::JmapMethodTransportResult>
        call(javelin::jmap::api::JmapMethodRequest value) override
        {
            requests.push_back(value);
            request = std::move(value);
            REQUIRE_FALSE(results.empty());
            auto result = std::move(results.front());
            results.erase(results.begin());
            if (auto callback = std::exchange(beforeReturn, {}))
                callback();
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
        value.capabilities.coreDetails = javelin::jmap::api::CoreCapability{};
        value.capabilities.coreDetails->maxObjectsInGet = 1;
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
                                                             .defaultAlertsWithTime = {},
                                                             .defaultAlertsWithoutTime = {},
                                                             .myRights = {.mayReadFreeBusy = true,
                                                                          .mayReadItems = true,
                                                                          .mayWriteAll = true,
                                                                          .mayWriteOwn = true,
                                                                          .mayUpdatePrivate = true,
                                                                          .mayRSVP = true,
                                                                          .mayShare = false,
                                                                          .mayDelete = false}}})
                      .has_value());
    auto cachedEvent = event();
    cachedEvent.title = "Original";
    REQUIRE_FALSE(
        calendars
            .reconcileWindow({.accountId = "a1",
                              .start = {.value = "2026-06-29T00:00:00"},
                              .end = {.value = "2026-08-10T00:00:00"},
                              .displayTimeZone = {.value = "Pacific/Auckland"},
                              .queryState = "query-state-1",
                              .eventState = "event-state-7",
                              .events = {cachedEvent},
                              .occurrences = {{.accountId = "a1",
                                               .id = "event-1",
                                               .eventId = "event-1",
                                               .recurrenceId = std::nullopt,
                                               .localStart = {.value = "2026-07-13T09:00:00"},
                                               .localEnd = {.value = "2026-07-13T10:00:00"},
                                               .utcStart = std::nullopt,
                                               .utcEnd = std::nullopt,
                                               .allDay = false}}})
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
    transport.beforeReturn = [&connection, &calendars]
    {
        const auto projected = calendars.findEvent("a1", "event-1");
        REQUIRE(std::holds_alternative<std::optional<javelin::jmap::calendar::CalendarEvent>>(
            projected));
        REQUIRE(
            std::get<std::optional<javelin::jmap::calendar::CalendarEvent>>(projected).has_value());
        CHECK(std::get<std::optional<javelin::jmap::calendar::CalendarEvent>>(projected)->title ==
              "Updated");
        javelin::jmap::calendar::CalendarMutationJournal journal{connection, calendars};
        const auto records = journal.listForEvent("a1", "event-1");
        REQUIRE(
            std::holds_alternative<std::vector<javelin::jmap::calendar::CalendarMutationRecord>>(
                records));
        REQUIRE(std::get<std::vector<javelin::jmap::calendar::CalendarMutationRecord>>(records)
                    .size() == 1);
        CHECK(std::get<std::vector<javelin::jmap::calendar::CalendarMutationRecord>>(records)
                  .front()
                  .status == javelin::jmap::sync::MutationStatus::InFlight);
    };
    javelin::jmap::calendar::CalendarService service{connection, transport};
    std::size_t projectionNotifications = 0;
    const auto result =
        QCoro::waitFor(service.update({.sessionUrl = "https://example.test/.well-known/jmap",
                                       .loginEmail = "alice@example.test",
                                       .apiKey = "secret"},
                                      "a1",
                                      {.accountId = "a1",
                                       .event = event(),
                                       .operationGroupId = std::nullopt,
                                       .ifInState = std::nullopt,
                                       .materialization = std::nullopt},
                                      [&projectionNotifications] { ++projectionNotifications; }));

    REQUIRE(std::holds_alternative<javelin::jmap::calendar::CommittedMutation>(result));
    CHECK(projectionNotifications == 2);
    REQUIRE(transport.request.has_value());
    REQUIRE(transport.request->envelope.methodCalls.size() == 1);
    CHECK(transport.request->envelope.methodCalls.front().arguments.find(
              R"("ifInState":"event-state-7")") != std::string::npos);
    javelin::jmap::sync::ConsistencyDomainRepository consistency{connection};
    const auto mutationGeneration =
        consistency.mutationGeneration({.accountId = "a1", .dataType = "CalendarEvent"});
    REQUIRE(std::holds_alternative<std::uint64_t>(mutationGeneration));
    CHECK(std::get<std::uint64_t>(mutationGeneration) == 1);
    javelin::jmap::calendar::CalendarMutationJournal journal{connection, calendars};
    const auto records = journal.listForEvent("a1", "event-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::calendar::CalendarMutationRecord>>(
        records));
    CHECK(std::get<std::vector<javelin::jmap::calendar::CalendarMutationRecord>>(records).empty());

    transport.request.reset();
    const auto noOpUpdate =
        QCoro::waitFor(service.update({.sessionUrl = "https://example.test/.well-known/jmap",
                                       .loginEmail = "alice@example.test",
                                       .apiKey = "secret"},
                                      "a1",
                                      {.accountId = "a1",
                                       .event = event(),
                                       .operationGroupId = std::nullopt,
                                       .ifInState = std::nullopt,
                                       .materialization = std::nullopt},
                                      [&projectionNotifications] { ++projectionNotifications; }));
    REQUIRE(std::holds_alternative<javelin::jmap::calendar::CommittedMutation>(noOpUpdate));
    CHECK_FALSE(transport.request.has_value());
    CHECK(projectionNotifications == 2);

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

    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "Calendar/changes",
              .arguments =
                  R"({"accountId":"a1","oldState":"calendar-state-2","newState":"calendar-state-old","hasMoreChanges":false,"created":[],"updated":[],"destroyed":[]})",
              .callId = "calendar-changes"},
             {.name = "CalendarEvent/changes",
              .arguments =
                  R"({"accountId":"a1","oldState":"event-state-9","newState":"event-state-old","hasMoreChanges":false,"created":[],"updated":[],"destroyed":[]})",
              .callId = "calendar-event-changes"}},
        .createdIds = std::nullopt,
        .sessionState = "session-old"});
    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "Calendar/changes",
              .arguments =
                  R"({"accountId":"a1","oldState":"calendar-state-2","newState":"calendar-state-new","hasMoreChanges":false,"created":[],"updated":[],"destroyed":[]})",
              .callId = "calendar-changes"},
             {.name = "CalendarEvent/changes",
              .arguments =
                  R"({"accountId":"a1","oldState":"event-state-9","newState":"event-state-new","hasMoreChanges":false,"created":[],"updated":[],"destroyed":[]})",
              .callId = "calendar-event-changes"}},
        .createdIds = std::nullopt,
        .sessionState = "session-new"});
    const javelin::jmap::LiveConnectionSettings settings{
        .sessionUrl = "https://example.test/.well-known/jmap",
        .loginEmail = "alice@example.test",
        .apiKey = "secret"};
    const javelin::jmap::calendar::VisibleInterval interval{
        .start = {.value = "2026-06-29T00:00:00"}, .end = {.value = "2026-08-10T00:00:00"}};
    const javelin::jmap::calendar::TimeZoneId zone{.value = "Pacific/Auckland"};
    std::optional<javelin::jmap::calendar::CalendarRefreshResult> newerRefresh;
    transport.beforeReturn = [&]
    { newerRefresh = QCoro::waitFor(service.refreshChanged(settings, "a1", interval, zone)); };

    const auto superseded = QCoro::waitFor(service.refreshChanged(settings, "a1", interval, zone));

    REQUIRE(newerRefresh.has_value());
    REQUIRE(std::holds_alternative<javelin::jmap::calendar::RefreshedRange>(*newerRefresh));
    REQUIRE(std::holds_alternative<javelin::jmap::calendar::RefreshedRange>(superseded));
    CHECK(std::get<javelin::jmap::calendar::RefreshedRange>(superseded).accountCount == 0);
    const auto newestCalendarState = calendars.stateToken("a1", "Calendar");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(newestCalendarState));
    CHECK(std::get<std::optional<std::string>>(newestCalendarState) ==
          std::optional<std::string>{"calendar-state-new"});
    const auto newestEventState = calendars.stateToken("a1", "CalendarEvent");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(newestEventState));
    CHECK(std::get<std::optional<std::string>>(newestEventState) ==
          std::optional<std::string>{"event-state-new"});

    const auto requestCount = transport.requests.size();
    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "Calendar/get",
              .arguments =
                  R"({"accountId":"a1","state":"calendar-batched","list":[{"id":"work","name":"Work","isSubscribed":true,"isVisible":true,"isDefault":true,"myRights":{"mayReadFreeBusy":true,"mayReadItems":true,"mayWriteAll":true,"mayWriteOwn":true,"mayUpdatePrivate":true,"mayRSVP":true,"mayShare":false,"mayDelete":false}}],"notFound":[]})",
              .callId = "calendar-get"},
             {.name = "CalendarEvent/query",
              .arguments =
                  R"({"accountId":"a1","queryState":"expanded-query","canCalculateChanges":false,"position":0,"ids":["expanded-1","expanded-2"],"total":2,"limit":2})",
              .callId = "calendar-event-query"},
             {.name = "CalendarEvent/query",
              .arguments =
                  R"({"accountId":"a1","queryState":"base-query","canCalculateChanges":false,"position":0,"ids":["base-1","base-2"],"total":2,"limit":2})",
              .callId = "calendar-base-event-query"}},
        .createdIds = std::nullopt,
        .sessionState = "session-batched-query"});
    const auto getResponse = [](const std::string& callId, const std::string& id,
                                const std::string& uid, const std::string& start)
    {
        return javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {{.name = "CalendarEvent/get",
                  .arguments =
                      QStringLiteral(
                          R"({"accountId":"a1","state":"event-batched","list":[{"@type":"Event","id":"%1","uid":"%2","calendarIds":{"work":true},"title":"%1","start":"%3","duration":"PT1H","timeZone":"Pacific/Auckland","showWithoutTime":false,"isDraft":false,"isOrigin":true}],"notFound":[]})")
                          .arg(QString::fromStdString(id), QString::fromStdString(uid),
                               QString::fromStdString(start))
                          .toStdString(),
                  .callId = callId}},
            .createdIds = std::nullopt,
            .sessionState = "session-batched-get"};
    };
    transport.results.push_back(
        getResponse("calendar-event-get", "expanded-1", "expanded-uid-1", "2026-07-20T09:00:00"));
    transport.results.push_back(
        getResponse("calendar-event-get", "expanded-2", "expanded-uid-2", "2026-07-21T09:00:00"));
    transport.results.push_back(
        getResponse("calendar-base-event-get", "base-1", "base-uid-1", "2026-07-20T09:00:00"));
    transport.results.push_back(
        getResponse("calendar-base-event-get", "base-2", "base-uid-2", "2026-07-21T09:00:00"));

    const auto batched = QCoro::waitFor(service.refresh(settings, "a1", interval, zone));

    REQUIRE(std::holds_alternative<javelin::jmap::calendar::RefreshedRange>(batched));
    CHECK(std::get<javelin::jmap::calendar::RefreshedRange>(batched).eventCount == 2);
    REQUIRE(transport.requests.size() == requestCount + 5);
    for (auto index = requestCount + 1; index < transport.requests.size(); ++index)
    {
        REQUIRE(transport.requests[index].envelope.methodCalls.size() == 1);
        const auto& arguments = transport.requests[index].envelope.methodCalls.front().arguments;
        CHECK_FALSE(arguments.find(R"("expanded-1","expanded-2")") != std::string::npos);
        CHECK_FALSE(arguments.find(R"("base-1","base-2")") != std::string::npos);
    }

    const auto editable = event();
    REQUIRE_FALSE(calendars
                      .applyEventDelta("a1", "calendar-batched", "event-batched", zone, {editable},
                                       {{.accountId = "a1",
                                         .id = "event-1",
                                         .eventId = "event-1",
                                         .recurrenceId = std::nullopt,
                                         .localStart = {.value = "2026-07-13T09:00:00"},
                                         .localEnd = {.value = "2026-07-13T10:00:00"},
                                         .utcStart = std::nullopt,
                                         .utcEnd = std::nullopt,
                                         .allDay = false}},
                                       {})
                      .has_value());

    const auto cachedBeforeFailure = calendars.loadWindow("a1", interval.start, interval.end, zone);
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::CalendarWindow>>(
        cachedBeforeFailure));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::CalendarWindow>>(cachedBeforeFailure)
                .has_value());
    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "CalendarEvent/set",
              .arguments =
                  R"({"accountId":"a1","oldState":"event-batched","newState":"event-batched","created":{},"updated":{},"destroyed":[],"notCreated":{},"notUpdated":{"event-1":{"type":"forbidden","description":"read only"}},"notDestroyed":{}})",
              .callId = "calendar-event-set"}},
        .createdIds = std::nullopt,
        .sessionState = "session-forbidden"});

    auto forbiddenEvent = event();
    forbiddenEvent.title = "Forbidden update";
    const auto forbidden = QCoro::waitFor(service.update(settings, "a1",
                                                         {.accountId = "a1",
                                                          .event = std::move(forbiddenEvent),
                                                          .operationGroupId = std::nullopt,
                                                          .ifInState = std::nullopt,
                                                          .materialization = std::nullopt}));

    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(forbidden));
    CHECK(std::get<javelin::jmap::OperationError>(forbidden).code ==
          javelin::jmap::OperationErrorCode::PermissionDenied);
    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "CalendarEvent/set",
              .arguments =
                  R"({"accountId":"a1","oldState":"event-batched","newState":"event-batched","created":{},"updated":{},"destroyed":[],"notCreated":{"event-1":{"type":"noSupportedScheduleMethods","description":"recipient unsupported"}},"notUpdated":{},"notDestroyed":{}})",
              .callId = "calendar-event-set"}},
        .createdIds = std::nullopt,
        .sessionState = "session-scheduling-failure"});
    auto scheduledEvent = event();
    scheduledEvent.id.clear();
    scheduledEvent.uid.clear();

    const auto scheduling = QCoro::waitFor(service.create(settings, "a1",
                                                          {.accountId = "a1",
                                                           .event = scheduledEvent,
                                                           .operationGroupId = std::nullopt,
                                                           .ifInState = std::nullopt,
                                                           .materialization = std::nullopt}));

    REQUIRE_FALSE(transport.requests.empty());
    REQUIRE(transport.requests.back().envelope.methodCalls.size() == 1);
    const auto& createArguments = transport.requests.back().envelope.methodCalls.front().arguments;
    CHECK(createArguments.find(R"("uid":")") != std::string::npos);
    CHECK(createArguments.find(R"("uid":"")") == std::string::npos);
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(scheduling));
    CHECK(std::get<javelin::jmap::OperationError>(scheduling).code ==
          javelin::jmap::OperationErrorCode::SchedulingUnsupported);
    const auto cachedAfterFailure = calendars.loadWindow("a1", interval.start, interval.end, zone);
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::CalendarWindow>>(
        cachedAfterFailure));
    const auto& before =
        *std::get<std::optional<javelin::jmap::cache::CalendarWindow>>(cachedBeforeFailure);
    const auto& after =
        *std::get<std::optional<javelin::jmap::cache::CalendarWindow>>(cachedAfterFailure);
    CHECK(after.eventState == before.eventState);
    REQUIRE(after.events.size() == before.events.size());
    CHECK(after.events.front().title == before.events.front().title);

    const auto rejectedRecords =
        javelin::jmap::calendar::CalendarMutationJournal{connection, calendars}.listForEvent(
            "a1", "event-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::calendar::CalendarMutationRecord>>(
        rejectedRecords));
    CHECK(std::ranges::any_of(
        std::get<std::vector<javelin::jmap::calendar::CalendarMutationRecord>>(rejectedRecords),
        [](const auto& record)
        { return record.status == javelin::jmap::sync::MutationStatus::Rejected; }));

    transport.results.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
        .message = "Connection closed after dispatch",
    });
    auto uncertainEvent = event();
    uncertainEvent.title = "Uncertain";
    uncertainEvent.recurrenceRule = javelin::jmap::calendar::RecurrenceRule{};
    uncertainEvent.recurrenceRule->count = 3;
    const auto uncertain = QCoro::waitFor(service.update(settings, "a1",
                                                         {.accountId = "a1",
                                                          .event = uncertainEvent,
                                                          .operationGroupId = std::nullopt,
                                                          .ifInState = std::nullopt,
                                                          .materialization = std::nullopt}));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(uncertain));
    const auto uncertainCached = calendars.findEvent("a1", "event-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::calendar::CalendarEvent>>(
        uncertainCached));
    REQUIRE(std::get<std::optional<javelin::jmap::calendar::CalendarEvent>>(uncertainCached)
                .has_value());
    CHECK(std::get<std::optional<javelin::jmap::calendar::CalendarEvent>>(uncertainCached)->title ==
          "Uncertain");
    const auto uncertainRecords =
        javelin::jmap::calendar::CalendarMutationJournal{connection, calendars}.listForEvent(
            "a1", "event-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::calendar::CalendarMutationRecord>>(
        uncertainRecords));
    CHECK(std::ranges::any_of(
        std::get<std::vector<javelin::jmap::calendar::CalendarMutationRecord>>(uncertainRecords),
        [](const auto& record)
        { return record.status == javelin::jmap::sync::MutationStatus::Unknown; }));

    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "Calendar/get",
              .arguments =
                  R"({"accountId":"a1","state":"calendar-stale","list":[{"id":"work","name":"Work","isSubscribed":true,"isVisible":true,"isDefault":true,"myRights":{"mayReadFreeBusy":true,"mayReadItems":true,"mayWriteAll":true,"mayWriteOwn":true,"mayUpdatePrivate":true,"mayRSVP":true,"mayShare":false,"mayDelete":false}}],"notFound":[]})",
              .callId = "calendar-get"},
             {.name = "CalendarEvent/query",
              .arguments =
                  R"({"accountId":"a1","queryState":"expanded-stale","canCalculateChanges":false,"position":0,"ids":["occurrence-stale"],"total":1,"limit":1})",
              .callId = "calendar-event-query"},
             {.name = "CalendarEvent/query",
              .arguments =
                  R"({"accountId":"a1","queryState":"base-stale","canCalculateChanges":false,"position":0,"ids":["event-1"],"total":1,"limit":1})",
              .callId = "calendar-base-event-query"}},
        .createdIds = std::nullopt,
        .sessionState = "session-stale-query"});
    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "CalendarEvent/get",
              .arguments =
                  R"({"accountId":"a1","state":"event-stale","list":[{"@type":"Event","id":"occurrence-stale","baseEventId":"event-1","recurrenceId":"2026-07-13T09:00:00","uid":"uid-1","calendarIds":{"work":true},"title":"Stale","start":"2026-07-13T09:00:00","duration":"PT1H","timeZone":"Pacific/Auckland","showWithoutTime":false,"isDraft":false,"isOrigin":true}],"notFound":[]})",
              .callId = "calendar-event-get"}},
        .createdIds = std::nullopt,
        .sessionState = "session-stale-expanded"});
    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "CalendarEvent/get",
              .arguments =
                  R"({"accountId":"a1","state":"event-stale","list":[{"@type":"Event","id":"event-1","uid":"uid-1","calendarIds":{"work":true},"title":"Stale","start":"2026-07-13T09:00:00","duration":"PT1H","timeZone":"Pacific/Auckland","showWithoutTime":false,"isDraft":false,"isOrigin":true,"recurrenceRule":{"@type":"RecurrenceRule","frequency":"daily","interval":1,"count":3}}],"notFound":[]})",
              .callId = "calendar-base-event-get"}},
        .createdIds = std::nullopt,
        .sessionState = "session-stale-base"});

    const auto staleRefresh = QCoro::waitFor(service.refresh(settings, "a1", interval, zone));

    REQUIRE(std::holds_alternative<javelin::jmap::calendar::RefreshedRange>(staleRefresh));
    const auto rebasedWindow = calendars.loadWindow("a1", interval.start, interval.end, zone);
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::CalendarWindow>>(rebasedWindow));
    const auto& rebased =
        std::get<std::optional<javelin::jmap::cache::CalendarWindow>>(rebasedWindow);
    REQUIRE(rebased.has_value());
    REQUIRE(rebased->events.size() == 1);
    CHECK(rebased->events.front().title == "Uncertain");
    REQUIRE(rebased->occurrences.size() == 1);
    CHECK(rebased->occurrences.front().id == "event-1");
    CHECK(rebased->occurrences.front().eventId == "event-1");

    auto listedCalendars = calendars.listCalendars("a1");
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::calendar::Calendar>>(listedCalendars));
    auto calendarDocuments =
        std::get<std::vector<javelin::jmap::calendar::Calendar>>(listedCalendars);
    REQUIRE(calendarDocuments.size() == 1);
    auto readOnly = calendarDocuments.front();
    readOnly.id = "read-only";
    readOnly.name = "Read only";
    readOnly.isDefault = false;
    readOnly.myRights.mayWriteAll = false;
    readOnly.myRights.mayWriteOwn = false;
    calendarDocuments.push_back(readOnly);
    auto personal = calendarDocuments.front();
    personal.id = "personal";
    personal.name = "Personal";
    personal.isDefault = false;
    calendarDocuments.push_back(personal);
    REQUIRE_FALSE(
        calendars.replaceCalendars("a1", "calendar-with-read-only", calendarDocuments).has_value());

    const auto readOnlyDefault =
        QCoro::waitFor(service.setDefaultCalendar(settings, "a1", "a1", "read-only"));

    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(readOnlyDefault));
    CHECK(std::get<javelin::jmap::OperationError>(readOnlyDefault).code ==
          javelin::jmap::OperationErrorCode::PermissionDenied);

    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "Calendar/set",
              .arguments =
                  R"({"accountId":"a1","oldState":"calendar-with-read-only","newState":"calendar-default-2","updated":{},"notUpdated":{}})",
              .callId = "calendar-set-default"}},
        .createdIds = std::nullopt,
        .sessionState = "session-calendar-default"});
    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "Calendar/get",
              .arguments =
                  R"({"accountId":"a1","state":"calendar-default-verified","list":[{"id":"personal","name":"Personal","isSubscribed":true,"isVisible":true,"isDefault":true,"myRights":{"mayReadFreeBusy":true,"mayReadItems":true,"mayWriteAll":true,"mayWriteOwn":true,"mayUpdatePrivate":true,"mayRSVP":true,"mayShare":false,"mayDelete":false}}],"notFound":[]})",
              .callId = "calendar-get-default-verification"}},
        .createdIds = std::nullopt,
        .sessionState = "session-calendar-default-verified"});
    const auto changedDefault =
        QCoro::waitFor(service.setDefaultCalendar(settings, "a1", "a1", "personal"));
    REQUIRE(std::holds_alternative<javelin::jmap::calendar::CommittedMutation>(changedDefault));
    REQUIRE(transport.requests.size() >= 2);
    const auto& setDefaultRequest = transport.requests[transport.requests.size() - 2];
    REQUIRE(setDefaultRequest.envelope.methodCalls.size() == 1);
    CHECK(setDefaultRequest.envelope.methodCalls.front().name == "Calendar/set");
    CHECK(setDefaultRequest.envelope.methodCalls.front().arguments.find(
              R"("onSuccessSetIsDefault":"personal")") != std::string::npos);
    REQUIRE(transport.request.has_value());
    REQUIRE(transport.request->envelope.methodCalls.size() == 1);
    CHECK(transport.request->envelope.methodCalls.front().name == "Calendar/get");
    const auto defaultState = calendars.stateToken("a1", "Calendar");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(defaultState));
    CHECK(std::get<std::optional<std::string>>(defaultState) ==
          std::optional<std::string>{"calendar-default-verified"});
    const auto calendarsAfterDefault = calendars.listCalendars("a1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::calendar::Calendar>>(
        calendarsAfterDefault));
    const auto& defaultDocuments =
        std::get<std::vector<javelin::jmap::calendar::Calendar>>(calendarsAfterDefault);
    CHECK(std::ranges::count_if(defaultDocuments,
                                [](const auto& item) { return item.isDefault; }) == 1);
    const auto personalDefault =
        std::ranges::find(defaultDocuments, "personal", &javelin::jmap::calendar::Calendar::id);
    REQUIRE(personalDefault != defaultDocuments.end());
    CHECK(personalDefault->isDefault);

    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "Calendar/set",
              .arguments =
                  R"({"accountId":"a1","oldState":"calendar-default-verified","newState":"calendar-default-verified","updated":{},"notUpdated":{}})",
              .callId = "calendar-set-default"}},
        .createdIds = std::nullopt,
        .sessionState = "session-calendar-default-ignored"});
    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "Calendar/get",
              .arguments =
                  R"({"accountId":"a1","state":"calendar-default-verified","list":[{"id":"work","name":"Work","isSubscribed":true,"isVisible":true,"isDefault":false,"myRights":{"mayReadFreeBusy":true,"mayReadItems":true,"mayWriteAll":true,"mayWriteOwn":true,"mayUpdatePrivate":true,"mayRSVP":true,"mayShare":false,"mayDelete":false}}],"notFound":[]})",
              .callId = "calendar-get-default-verification"}},
        .createdIds = std::nullopt,
        .sessionState = "session-calendar-default-ignored-verification"});
    const auto ignoredDefault =
        QCoro::waitFor(service.setDefaultCalendar(settings, "a1", "a1", "work"));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(ignoredDefault));
    CHECK(std::get<javelin::jmap::OperationError>(ignoredDefault).code ==
          javelin::jmap::OperationErrorCode::PermissionDenied);

    transport.results.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
        .message = "Connection closed after Calendar/set dispatch",
    });
    const auto uncertainDefault =
        QCoro::waitFor(service.setDefaultCalendar(settings, "a1", "a1", "work"));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(uncertainDefault));
    javelin::jmap::sync::MutationJournalRepository genericJournal{connection};
    const auto activeDefaults =
        genericJournal.listActive({.accountId = "a1", .dataType = "Calendar"});
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::sync::MutationRecord>>(activeDefaults));
    REQUIRE(std::get<std::vector<javelin::jmap::sync::MutationRecord>>(activeDefaults).size() == 1);
    CHECK(
        std::get<std::vector<javelin::jmap::sync::MutationRecord>>(activeDefaults).front().status ==
        javelin::jmap::sync::MutationStatus::Unknown);
    const auto duplicateDefault =
        QCoro::waitFor(service.setDefaultCalendar(settings, "a1", "a1", "work"));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(duplicateDefault));
    CHECK(std::get<javelin::jmap::OperationError>(duplicateDefault).code ==
          javelin::jmap::OperationErrorCode::Conflict);

    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "Calendar/get",
              .arguments =
                  R"({"accountId":"a1","state":"calendar-default-resolved","list":[{"id":"work","name":"Work","isSubscribed":true,"isVisible":true,"isDefault":true,"myRights":{"mayReadFreeBusy":true,"mayReadItems":true,"mayWriteAll":true,"mayWriteOwn":true,"mayUpdatePrivate":true,"mayRSVP":true,"mayShare":false,"mayDelete":false}},{"id":"read-only","name":"Read only","isSubscribed":true,"isVisible":true,"isDefault":false,"myRights":{"mayReadFreeBusy":true,"mayReadItems":true,"mayWriteAll":false,"mayWriteOwn":false,"mayUpdatePrivate":false,"mayRSVP":false,"mayShare":false,"mayDelete":false}},{"id":"personal","name":"Personal","isSubscribed":true,"isVisible":true,"isDefault":false,"myRights":{"mayReadFreeBusy":true,"mayReadItems":true,"mayWriteAll":true,"mayWriteOwn":true,"mayUpdatePrivate":true,"mayRSVP":true,"mayShare":false,"mayDelete":false}}],"notFound":[]})",
              .callId = "calendar-get"},
             {.name = "CalendarEvent/query",
              .arguments =
                  R"({"accountId":"a1","queryState":"resolved-expanded","canCalculateChanges":false,"position":0,"ids":[],"total":0,"limit":1})",
              .callId = "calendar-event-query"},
             {.name = "CalendarEvent/query",
              .arguments =
                  R"({"accountId":"a1","queryState":"resolved-base","canCalculateChanges":false,"position":0,"ids":[],"total":0,"limit":1})",
              .callId = "calendar-base-event-query"}},
        .createdIds = std::nullopt,
        .sessionState = "session-calendar-default-resolved"});

    const auto resolvedDefault = QCoro::waitFor(service.refresh(settings, "a1", interval, zone));

    REQUIRE(std::holds_alternative<javelin::jmap::calendar::RefreshedRange>(resolvedDefault));
    const auto activeAfterResolution =
        genericJournal.listActive({.accountId = "a1", .dataType = "Calendar"});
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::MutationRecord>>(
        activeAfterResolution));
    CHECK(
        std::get<std::vector<javelin::jmap::sync::MutationRecord>>(activeAfterResolution).empty());
    const auto resolvedState = calendars.stateToken("a1", "Calendar");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(resolvedState));
    CHECK(std::get<std::optional<std::string>>(resolvedState) ==
          std::optional<std::string>{"calendar-default-resolved"});
}

TEST_CASE("calendar refresh recovers a recurring base omitted by the bounded base query",
          "[jmap][calendar][service][recurrence]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-service-recover-base"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    if (const auto error = sessions.replace("a1", session()))
        FAIL(error->message.toStdString());

    FakeMethodTransport transport;
    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "Calendar/get",
              .arguments =
                  R"({"accountId":"a1","state":"calendar-water","list":[{"id":"work","name":"Work","isSubscribed":true,"isVisible":true,"isDefault":true,"myRights":{"mayReadFreeBusy":true,"mayReadItems":true,"mayWriteAll":true,"mayWriteOwn":true,"mayUpdatePrivate":true,"mayRSVP":true,"mayShare":false,"mayDelete":false}}],"notFound":[]})",
              .callId = "calendar-get"},
             {.name = "CalendarEvent/query",
              .arguments =
                  R"({"accountId":"a1","queryState":"expanded-water","canCalculateChanges":false,"position":0,"ids":["synthetic-water"],"total":1})",
              .callId = "calendar-event-query"},
             {.name = "CalendarEvent/query",
              .arguments =
                  R"({"accountId":"a1","queryState":"base-water-missing","canCalculateChanges":false,"position":0,"ids":[],"total":0})",
              .callId = "calendar-base-event-query"}},
        .createdIds = std::nullopt,
        .sessionState = "session-water-query"});
    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "CalendarEvent/get",
              .arguments =
                  R"({"accountId":"a1","state":"event-water","list":[{"@type":"Event","id":"synthetic-water","recurrenceId":"2026-07-01T02:20:00","uid":"water-series-uid","calendarIds":{"work":true},"title":"Water Softener Running","start":"2026-07-01T02:20:00","duration":"PT2H10M","timeZone":"Pacific/Auckland","showWithoutTime":false,"isDraft":false,"isOrigin":true}],"notFound":[]})",
              .callId = "calendar-event-get"}},
        .createdIds = std::nullopt,
        .sessionState = "session-water-expanded-get"});
    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "CalendarEvent/get",
              .arguments = R"({"accountId":"a1","state":"event-water","list":[],"notFound":[]})",
              .callId = "calendar-base-event-get"}},
        .createdIds = std::nullopt,
        .sessionState = "session-water-empty-base-get"});
    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "CalendarEvent/query",
              .arguments =
                  R"({"accountId":"a1","queryState":"base-water-recovered","canCalculateChanges":false,"position":0,"ids":["base-water"],"total":1})",
              .callId = "calendar-base-event-recovery-query"}},
        .createdIds = std::nullopt,
        .sessionState = "session-water-recovery-query"});
    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "CalendarEvent/get",
              .arguments =
                  R"({"accountId":"a1","state":"event-water","list":[{"@type":"Event","id":"base-water","uid":"water-series-uid","calendarIds":{"work":true},"title":"Water Softener Running","start":"2026-01-08T02:20:00","duration":"PT2H10M","timeZone":"Pacific/Auckland","showWithoutTime":false,"isDraft":false,"isOrigin":true,"recurrenceRule":{"@type":"RecurrenceRule","frequency":"daily","interval":3}}],"notFound":[]})",
              .callId = "calendar-base-event-recovery-get"}},
        .createdIds = std::nullopt,
        .sessionState = "session-water-recovery-get"});

    javelin::jmap::calendar::CalendarService service{connection, transport};
    const javelin::jmap::calendar::VisibleInterval interval{
        .start = {.value = "2026-06-29T00:00:00"}, .end = {.value = "2026-08-10T00:00:00"}};
    const javelin::jmap::calendar::TimeZoneId zone{.value = "Pacific/Auckland"};
    const auto refreshed =
        QCoro::waitFor(service.refresh({.sessionUrl = "https://example.test/.well-known/jmap",
                                        .loginEmail = "alice@example.test",
                                        .apiKey = "secret"},
                                       "a1", interval, zone));

    REQUIRE(std::holds_alternative<javelin::jmap::calendar::RefreshedRange>(refreshed));
    javelin::jmap::cache::CalendarRepository calendars{connection};
    const auto loaded = calendars.loadWindow("a1", interval.start, interval.end, zone);
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::CalendarWindow>>(loaded));
    const auto& window = std::get<std::optional<javelin::jmap::cache::CalendarWindow>>(loaded);
    REQUIRE(window.has_value());
    REQUIRE(window->events.size() == 1);
    CHECK(window->events.front().id == "base-water");
    CHECK(window->events.front().title == "Water Softener Running");
    REQUIRE(window->occurrences.size() == 1);
    CHECK(window->occurrences.front().eventId == "base-water");
    CHECK(window->occurrences.front().localStart.value == "2026-07-01T02:20:00");
    REQUIRE(transport.requests.size() == 5);
    REQUIRE(transport.requests[1].envelope.methodCalls.size() == 1);
    CHECK(transport.requests[1].envelope.methodCalls.front().arguments.find(R"("properties":[)") !=
          std::string::npos);
    CHECK(transport.requests[1].envelope.methodCalls.front().arguments.find(R"("baseEventId")") !=
          std::string::npos);
    CHECK(transport.requests[3].envelope.methodCalls.front().arguments.find(
              R"("uid":"water-series-uid")") != std::string::npos);
    CHECK(transport.requests[3].envelope.methodCalls.front().arguments.find(R"("after")") ==
          std::string::npos);
    CHECK(transport.requests[3].envelope.methodCalls.front().arguments.find(R"("before")") ==
          std::string::npos);
}
