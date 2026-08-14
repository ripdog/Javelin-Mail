#include "jmap/calendar/CalendarCacheReader.h"
#include "jmap/calendar/CalendarMutationEngine.h"
#include "jmap/calendar/CalendarProtocolClient.h"
#include "jmap/calendar/CalendarSyncEngine.h"

#include "jmap/api/JmapMethodTransport.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/calendar/CalendarMutationJournal.h"
#include "jmap/sync/ConsistencyDomain.h"
#include "jmap/sync/MutationJournal.h"

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
                                              .submission = std::nullopt,
                                              .contacts = std::nullopt,
                                              .calendars = javelin::jmap::api::CalendarsCapability{
                                                  .maxCalendarsPerEvent = 4,
                                                  .minDateTime = "1900-01-01T00:00:00Z",
                                                  .maxDateTime = "2100-01-01T00:00:00Z",
                                                  .maxExpandedQueryDuration = "P1Y",
                                                  .maxParticipantsPerEvent = 100,
                                                  .mayCreateCalendar = true}}});
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
        value.isOrigin = true;
        return value;
    }
} // namespace

TEST_CASE("calendar subscriptions project and reconcile server outcomes",
          "[jmap][calendar][subscriptions]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-subscription-mutation"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("a1", session()).has_value());
    javelin::jmap::cache::CalendarRepository calendars{connection};
    const javelin::jmap::calendar::Calendar work{
        .accountId = "a1",
        .id = "work",
        .name = "Work",
        .description = std::nullopt,
        .color = "#2457a6",
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
                     .mayDelete = false},
    };
    REQUIRE_FALSE(calendars.replaceCalendars("a1", "c1", {work}).has_value());
    FakeMethodTransport transport;
    javelin::jmap::calendar::CalendarCacheReader reader{connection};
    javelin::jmap::calendar::CalendarProtocolClient protocol{connection, transport};
    javelin::jmap::calendar::CalendarSyncEngine sync{connection, protocol};
    javelin::jmap::calendar::CalendarMutationEngine mutation{connection, protocol, sync, reader};
    const javelin::jmap::LiveConnectionSettings settings{
        .sessionUrl = "https://example.test/.well-known/jmap",
        .loginEmail = "alice@example.test",
        .apiKey = "secret"};

    const auto subscribed = [&calendars]
    {
        const auto listed = calendars.listCalendars("a1");
        REQUIRE(std::holds_alternative<std::vector<javelin::jmap::calendar::Calendar>>(listed));
        const auto& values = std::get<std::vector<javelin::jmap::calendar::Calendar>>(listed);
        REQUIRE(values.size() == 1);
        return values.front().isSubscribed;
    };

    SECTION("accepted update commits the projected subscription")
    {
        transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {{.name = "Calendar/set",
                  .arguments =
                      R"({"accountId":"a1","oldState":"c1","newState":"c2","created":{},"updated":{"work":{}},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{}})",
                  .callId = "calendar-set-subscription"}},
            .createdIds = std::nullopt,
            .sessionState = "s2"});
        transport.beforeReturn = [&subscribed] { CHECK_FALSE(subscribed()); };

        const auto result =
            QCoro::waitFor(mutation.setCalendarSubscribed(settings, "a1", "a1", "work", false));

        REQUIRE(std::holds_alternative<javelin::jmap::calendar::CommittedMutation>(result));
        CHECK_FALSE(subscribed());
        REQUIRE(transport.request.has_value());
        CHECK(transport.request->envelope.methodCalls.front().arguments.find(
                  R"("update":{"work":{"isSubscribed":false}})") != std::string::npos);
    }

    SECTION("definitive rejection restores the confirmed subscription")
    {
        transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {{.name = "Calendar/set",
                  .arguments =
                      R"({"accountId":"a1","oldState":"c1","newState":"c1","created":{},"updated":{},"destroyed":[],"notCreated":{},"notUpdated":{"work":{"type":"forbidden","description":"protected"}},"notDestroyed":{}})",
                  .callId = "calendar-set-subscription"}},
            .createdIds = std::nullopt,
            .sessionState = "s2"});
        transport.beforeReturn = [&subscribed] { CHECK_FALSE(subscribed()); };

        const auto result =
            QCoro::waitFor(mutation.setCalendarSubscribed(settings, "a1", "a1", "work", false));

        REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
        CHECK(subscribed());
    }

    SECTION("ambiguous transport preserves the projected subscription as unknown")
    {
        transport.results.push_back(javelin::jmap::api::TransportError{
            .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
            .message = "Connection closed after Calendar/set dispatch",
        });

        const auto result =
            QCoro::waitFor(mutation.setCalendarSubscribed(settings, "a1", "a1", "work", false));

        REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
        CHECK_FALSE(subscribed());
        javelin::jmap::sync::MutationJournalRepository journal{connection};
        const auto active = journal.listActive({.accountId = "a1", .dataType = "Calendar"});
        REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::MutationRecord>>(active));
        REQUIRE(std::get<std::vector<javelin::jmap::sync::MutationRecord>>(active).size() == 1);
        CHECK(std::get<std::vector<javelin::jmap::sync::MutationRecord>>(active).front().status ==
              javelin::jmap::sync::MutationStatus::Unknown);

        transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {{.name = "Calendar/get",
                  .arguments =
                      R"({"accountId":"a1","state":"c2","list":[{"id":"work","name":"Work","color":"#2457a6","sortOrder":0,"isSubscribed":false,"isVisible":true,"isDefault":true,"myRights":{"mayReadFreeBusy":true,"mayReadItems":true,"mayWriteAll":true,"mayWriteOwn":true,"mayUpdatePrivate":true,"mayRSVP":true,"mayShare":false,"mayDelete":false}}],"notFound":[]})",
                  .callId = "calendar-get"},
                 {.name = "CalendarEvent/query",
                  .arguments =
                      R"({"accountId":"a1","queryState":"q2","canCalculateChanges":false,"position":0,"ids":[],"total":0,"limit":100})",
                  .callId = "calendar-event-query"},
                 {.name = "CalendarEvent/query",
                  .arguments =
                      R"({"accountId":"a1","queryState":"qb2","canCalculateChanges":false,"position":0,"ids":[],"total":0,"limit":100})",
                  .callId = "calendar-base-event-query"}},
            .createdIds = std::nullopt,
            .sessionState = "s3"});
        const auto refreshed = QCoro::waitFor(sync.refresh(
            settings, "a1",
            {.start = {.value = "2026-08-01T00:00:00"}, .end = {.value = "2026-09-01T00:00:00"}},
            {.value = "Pacific/Auckland"}));
        REQUIRE(std::holds_alternative<javelin::jmap::calendar::RefreshedRange>(refreshed));
        const auto resolved = journal.listActive({.accountId = "a1", .dataType = "Calendar"});
        REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::MutationRecord>>(resolved));
        CHECK(std::get<std::vector<javelin::jmap::sync::MutationRecord>>(resolved).empty());
        CHECK_FALSE(subscribed());
    }
}

TEST_CASE("calendar manager mutations project, reconcile, and preserve uncertainty",
          "[jmap][calendar][service]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-manager-mutations"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("a1", session()).has_value());
    javelin::jmap::cache::CalendarRepository calendars{connection};
    const javelin::jmap::calendar::Calendar work{
        .accountId = "a1",
        .id = "work",
        .name = "Work",
        .description = std::nullopt,
        .color = "#2457a6",
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
                     .mayShare = true,
                     .mayDelete = true},
    };
    REQUIRE_FALSE(calendars.replaceCalendars("a1", "c1", {work}).has_value());
    FakeMethodTransport transport;
    javelin::jmap::calendar::CalendarCacheReader reader{connection};
    javelin::jmap::calendar::CalendarProtocolClient protocol{connection, transport};
    javelin::jmap::calendar::CalendarSyncEngine sync{connection, protocol};
    javelin::jmap::calendar::CalendarMutationEngine mutation{connection, protocol, sync, reader};
    const javelin::jmap::LiveConnectionSettings settings{
        .sessionUrl = "https://example.test/.well-known/jmap",
        .loginEmail = "alice@example.test",
        .apiKey = "secret"};

    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "Calendar/set",
              .arguments =
                  R"({"accountId":"a1","oldState":"c1","newState":"c2","created":{"new-calendar":{"id":"projects","isDefault":false}},"updated":{},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{}})",
              .callId = "calendar-set-manager"}},
        .createdIds = std::nullopt,
        .sessionState = "s2"});
    transport.beforeReturn = [&calendars]
    {
        const auto listed = calendars.listCalendars("a1");
        const auto& projected = std::get<std::vector<javelin::jmap::calendar::Calendar>>(listed);
        CHECK(std::ranges::find(projected, "Projects", &javelin::jmap::calendar::Calendar::name) !=
              projected.end());
    };
    auto created = QCoro::waitFor(mutation.createCalendar(
        settings, "a1", {.accountId = "a1", .name = "Projects", .color = "#336699"}));
    REQUIRE(std::holds_alternative<javelin::jmap::calendar::CommittedMutation>(created));
    CHECK(std::get<javelin::jmap::calendar::CommittedMutation>(created).createdId ==
          std::optional<std::string>{"projects"});

    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "Calendar/set",
              .arguments =
                  R"({"accountId":"a1","oldState":"c2","newState":"c2","created":{},"updated":{},"destroyed":[],"notCreated":{"new-calendar":{"type":"invalidProperties","description":"bad name","properties":["name"]}},"notUpdated":{},"notDestroyed":{}})",
              .callId = "calendar-set-manager"}},
        .createdIds = std::nullopt,
        .sessionState = "s3"});
    const auto rejectedCreate = QCoro::waitFor(mutation.createCalendar(
        settings, "a1", {.accountId = "a1", .name = "Rejected", .color = std::nullopt}));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(rejectedCreate));
    auto listed = calendars.listCalendars("a1");
    CHECK(std::ranges::find(std::get<std::vector<javelin::jmap::calendar::Calendar>>(listed),
                            "Rejected", &javelin::jmap::calendar::Calendar::name) ==
          std::get<std::vector<javelin::jmap::calendar::Calendar>>(listed).end());

    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "Calendar/set",
              .arguments =
                  R"({"accountId":"a1","oldState":"c2","newState":"c2","created":{},"updated":{},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{"work":{"type":"forbidden","description":"protected"}}})",
              .callId = "calendar-set-manager"}},
        .createdIds = std::nullopt,
        .sessionState = "s4"});
    transport.beforeReturn = [&calendars]
    {
        const auto projected = calendars.listCalendars("a1");
        CHECK(std::ranges::find(std::get<std::vector<javelin::jmap::calendar::Calendar>>(projected),
                                "work", &javelin::jmap::calendar::Calendar::id) ==
              std::get<std::vector<javelin::jmap::calendar::Calendar>>(projected).end());
    };
    const auto rejectedDelete = QCoro::waitFor(mutation.deleteCalendar(
        settings, "a1", {.accountId = "a1", .calendarId = "work", .removeEvents = true}));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(rejectedDelete));
    listed = calendars.listCalendars("a1");
    CHECK(std::ranges::find(std::get<std::vector<javelin::jmap::calendar::Calendar>>(listed),
                            "work", &javelin::jmap::calendar::Calendar::id) !=
          std::get<std::vector<javelin::jmap::calendar::Calendar>>(listed).end());

    transport.results.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
        .message = "Connection closed after Calendar/set dispatch",
    });
    const auto uncertainDelete = QCoro::waitFor(mutation.deleteCalendar(
        settings, "a1", {.accountId = "a1", .calendarId = "work", .removeEvents = true}));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(uncertainDelete));
    listed = calendars.listCalendars("a1");
    CHECK(std::ranges::find(std::get<std::vector<javelin::jmap::calendar::Calendar>>(listed),
                            "work", &javelin::jmap::calendar::Calendar::id) ==
          std::get<std::vector<javelin::jmap::calendar::Calendar>>(listed).end());
    javelin::jmap::sync::MutationJournalRepository journal{connection};
    const auto active = journal.listActive({.accountId = "a1", .dataType = "Calendar"});
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::MutationRecord>>(active));
    REQUIRE(std::get<std::vector<javelin::jmap::sync::MutationRecord>>(active).size() == 1);
    CHECK(std::get<std::vector<javelin::jmap::sync::MutationRecord>>(active).front().status ==
          javelin::jmap::sync::MutationStatus::Unknown);
    const auto duplicate = QCoro::waitFor(mutation.createCalendar(
        settings, "a1", {.accountId = "a1", .name = "Duplicate", .color = std::nullopt}));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(duplicate));
    CHECK(std::get<javelin::jmap::OperationError>(duplicate).code ==
          javelin::jmap::OperationErrorCode::Conflict);
}

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
    javelin::jmap::calendar::CalendarCacheReader reader{connection};
    javelin::jmap::calendar::CalendarProtocolClient protocol{connection, transport};
    javelin::jmap::calendar::CalendarSyncEngine sync{connection, protocol};
    javelin::jmap::calendar::CalendarMutationEngine mutation{connection, protocol, sync, reader};
    std::size_t projectionNotifications = 0;
    const auto result =
        QCoro::waitFor(mutation.update({.sessionUrl = "https://example.test/.well-known/jmap",
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
        QCoro::waitFor(mutation.update({.sessionUrl = "https://example.test/.well-known/jmap",
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
    const auto unchanged = QCoro::waitFor(sync.refreshChanged(
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
    const auto changed = QCoro::waitFor(sync.refreshChanged(
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
    { newerRefresh = QCoro::waitFor(sync.refreshChanged(settings, "a1", interval, zone)); };

    const auto superseded = QCoro::waitFor(sync.refreshChanged(settings, "a1", interval, zone));

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

    const auto batched = QCoro::waitFor(sync.refresh(settings, "a1", interval, zone));

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
    const auto forbidden = QCoro::waitFor(mutation.update(settings, "a1",
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

    const auto scheduling = QCoro::waitFor(mutation.create(settings, "a1",
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
    const auto uncertain = QCoro::waitFor(mutation.update(settings, "a1",
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

    const auto staleRefresh = QCoro::waitFor(sync.refresh(settings, "a1", interval, zone));

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
        QCoro::waitFor(mutation.setDefaultCalendar(settings, "a1", "a1", "read-only"));

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
        QCoro::waitFor(mutation.setDefaultCalendar(settings, "a1", "a1", "personal"));
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
        QCoro::waitFor(mutation.setDefaultCalendar(settings, "a1", "a1", "work"));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(ignoredDefault));
    CHECK(std::get<javelin::jmap::OperationError>(ignoredDefault).code ==
          javelin::jmap::OperationErrorCode::PermissionDenied);

    transport.results.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
        .message = "Connection closed after Calendar/set dispatch",
    });
    const auto uncertainDefault =
        QCoro::waitFor(mutation.setDefaultCalendar(settings, "a1", "a1", "work"));
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
        QCoro::waitFor(mutation.setDefaultCalendar(settings, "a1", "a1", "work"));
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

    const auto resolvedDefault = QCoro::waitFor(sync.refresh(settings, "a1", interval, zone));

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

TEST_CASE("calendar invitation responses use participant identities and RSVP rights",
          "[jmap][calendar][service][scheduling]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-service-rsvp"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    if (const auto error = sessions.replace("a1", session()))
        FAIL(error->message.toStdString());

    javelin::jmap::cache::CalendarRepository calendars{connection};
    const javelin::jmap::calendar::Calendar work{
        .accountId = "a1",
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
                     .mayWriteAll = false,
                     .mayWriteOwn = false,
                     .mayUpdatePrivate = true,
                     .mayRSVP = true,
                     .mayShare = false,
                     .mayDelete = false},
    };
    REQUIRE_FALSE(calendars.replaceCalendars("a1", "calendar-state-1", {work}).has_value());

    auto invitation = event();
    invitation.title = "Invitation";
    invitation.isOrigin = false;
    invitation.organizerCalendarAddress = "mailto:organizer@example.test";
    invitation.attendees = {
        {.id = "organizer",
         .name = "Organizer",
         .email = "organizer@example.test",
         .calendarAddress = "mailto:organizer@example.test",
         .participationStatus = "accepted",
         .isOwner = true,
         .isAttendee = true,
         .roles = {},
         .expectReply = false,
         .scheduleSequence = 0,
         .scheduleUpdated = std::nullopt},
        {.id = "me",
         .name = "Alice",
         .email = "alice@example.test",
         .calendarAddress = "mailto:alice@example.test",
         .participationStatus = "needs-action",
         .isOwner = false,
         .isAttendee = true,
         .roles = {},
         .expectReply = true,
         .scheduleSequence = 0,
         .scheduleUpdated = std::nullopt},
    };
    REQUIRE_FALSE(
        calendars
            .reconcileWindow({.accountId = "a1",
                              .start = {.value = "2026-06-29T00:00:00"},
                              .end = {.value = "2026-08-10T00:00:00"},
                              .displayTimeZone = {.value = "Pacific/Auckland"},
                              .queryState = "query-rsvp",
                              .eventState = "event-rsvp-1",
                              .events = {invitation},
                              .occurrences = {{.accountId = "a1",
                                               .id = "event-1",
                                               .eventId = "event-1",
                                               .recurrenceId = std::nullopt,
                                               .localStart = invitation.start,
                                               .localEnd = {.value = "2026-07-13T10:00:00"},
                                               .utcStart = std::nullopt,
                                               .utcEnd = std::nullopt,
                                               .allDay = false}}})
            .has_value());

    FakeMethodTransport transport;
    javelin::jmap::calendar::CalendarCacheReader reader{connection};
    javelin::jmap::calendar::CalendarProtocolClient protocol{connection, transport};
    javelin::jmap::calendar::CalendarSyncEngine sync{connection, protocol};
    javelin::jmap::calendar::CalendarMutationEngine mutation{connection, protocol, sync, reader};
    const javelin::jmap::LiveConnectionSettings settings{
        .sessionUrl = "https://example.test/.well-known/jmap",
        .loginEmail = "alice@example.test",
        .apiKey = "secret",
    };
    const auto identityResponse = [](const std::string& address)
    {
        return javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {{.name = "ParticipantIdentity/get",
                  .arguments =
                      QStringLiteral(
                          R"({"accountId":"a1","state":"participant-1","list":[{"id":"default","name":"Alice","calendarAddress":"%1","isDefault":true}],"notFound":[]})")
                          .arg(QString::fromStdString(address))
                          .toStdString(),
                  .callId = "participant-identities"}},
            .createdIds = std::nullopt,
            .sessionState = "session-participants",
        };
    };

    SECTION("accepted response projects and commits without ordinary write rights")
    {
        transport.results.push_back(identityResponse("mailto:alice@example.test"));
        transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {{.name = "CalendarEvent/set",
                  .arguments =
                      R"({"accountId":"a1","oldState":"event-rsvp-1","newState":"event-rsvp-2","created":{},"updated":{"event-1":null},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{}})",
                  .callId = "calendar-event-set"}},
            .createdIds = std::nullopt,
            .sessionState = "session-rsvp-accepted",
        });
        std::size_t projectionNotifications = 0;

        const auto result = QCoro::waitFor(mutation.respond(settings, "a1",
                                                            {.accountId = "a1",
                                                             .eventId = "event-1",
                                                             .participationStatus = "accepted",
                                                             .ifInState = std::nullopt,
                                                             .materialization = std::nullopt},
                                                            [&projectionNotifications]
                                                            { ++projectionNotifications; }));

        REQUIRE(std::holds_alternative<javelin::jmap::calendar::CommittedMutation>(result));
        CHECK(projectionNotifications == 2);
        REQUIRE(transport.requests.size() == 2);
        CHECK(transport.requests[0].envelope.methodCalls.front().name == "ParticipantIdentity/get");
        CHECK(transport.requests[1].envelope.methodCalls.front().name == "CalendarEvent/set");
        const auto& arguments = transport.requests[1].envelope.methodCalls.front().arguments;
        CHECK(arguments.find(R"("participants/me/participationStatus":"accepted")") !=
              std::string::npos);
        CHECK(arguments.find(R"("sendSchedulingMessages":true)") != std::string::npos);
        const auto cached = calendars.findEvent("a1", "event-1");
        REQUIRE(
            std::holds_alternative<std::optional<javelin::jmap::calendar::CalendarEvent>>(cached));
        const auto& accepted =
            std::get<std::optional<javelin::jmap::calendar::CalendarEvent>>(cached);
        REQUIRE(accepted.has_value());
        CHECK(accepted->attendees[1].participationStatus == "accepted");
    }

    SECTION("definitive RSVP rejection restores the invitation")
    {
        transport.results.push_back(identityResponse("mailto:alice@example.test"));
        transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {{.name = "CalendarEvent/set",
                  .arguments =
                      R"({"accountId":"a1","oldState":"event-rsvp-1","newState":"event-rsvp-1","created":{},"updated":{},"destroyed":[],"notCreated":{},"notUpdated":{"event-1":{"type":"forbidden","description":"RSVP disabled"}},"notDestroyed":{}})",
                  .callId = "calendar-event-set"}},
            .createdIds = std::nullopt,
            .sessionState = "session-rsvp-rejected",
        });

        const auto result = QCoro::waitFor(mutation.respond(settings, "a1",
                                                            {.accountId = "a1",
                                                             .eventId = "event-1",
                                                             .participationStatus = "declined",
                                                             .ifInState = std::nullopt,
                                                             .materialization = std::nullopt}));

        REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
        CHECK(std::get<javelin::jmap::OperationError>(result).code ==
              javelin::jmap::OperationErrorCode::PermissionDenied);
        const auto cached = calendars.findEvent("a1", "event-1");
        REQUIRE(
            std::holds_alternative<std::optional<javelin::jmap::calendar::CalendarEvent>>(cached));
        REQUIRE(
            std::get<std::optional<javelin::jmap::calendar::CalendarEvent>>(cached).has_value());
        CHECK(std::get<std::optional<javelin::jmap::calendar::CalendarEvent>>(cached)
                  ->attendees[1]
                  .participationStatus == "needs-action");
    }

    SECTION("ambiguous RSVP keeps the optimistic response as unknown")
    {
        transport.results.push_back(identityResponse("mailto:alice@example.test"));
        transport.results.push_back(javelin::jmap::api::TransportError{
            .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
            .message = "Connection closed after dispatch",
        });

        const auto result = QCoro::waitFor(mutation.respond(settings, "a1",
                                                            {.accountId = "a1",
                                                             .eventId = "event-1",
                                                             .participationStatus = "tentative",
                                                             .ifInState = std::nullopt,
                                                             .materialization = std::nullopt}));

        REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
        const auto cached = calendars.findEvent("a1", "event-1");
        REQUIRE(
            std::holds_alternative<std::optional<javelin::jmap::calendar::CalendarEvent>>(cached));
        REQUIRE(
            std::get<std::optional<javelin::jmap::calendar::CalendarEvent>>(cached).has_value());
        CHECK(std::get<std::optional<javelin::jmap::calendar::CalendarEvent>>(cached)
                  ->attendees[1]
                  .participationStatus == "tentative");
        const auto records =
            javelin::jmap::calendar::CalendarMutationJournal{connection, calendars}.listForEvent(
                "a1", "event-1");
        REQUIRE(
            std::holds_alternative<std::vector<javelin::jmap::calendar::CalendarMutationRecord>>(
                records));
        REQUIRE(std::get<std::vector<javelin::jmap::calendar::CalendarMutationRecord>>(records)
                    .size() == 1);
        CHECK(std::get<std::vector<javelin::jmap::calendar::CalendarMutationRecord>>(records)
                  .front()
                  .status == javelin::jmap::sync::MutationStatus::Unknown);
    }

    SECTION("an organizer update during identity lookup is preserved in the RSVP projection")
    {
        transport.results.push_back(identityResponse("mailto:alice@example.test"));
        transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {{.name = "CalendarEvent/set",
                  .arguments =
                      R"({"accountId":"a1","oldState":"event-rsvp-remote","newState":"event-rsvp-2","created":{},"updated":{"event-1":null},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{}})",
                  .callId = "calendar-event-set"}},
            .createdIds = std::nullopt,
            .sessionState = "session-rsvp-after-remote-update",
        });
        transport.beforeReturn = [&calendars, invitation]
        {
            auto remotelyUpdated = invitation;
            remotelyUpdated.title = "Organizer changed the title";
            const auto cacheError =
                calendars.applyEventDelta("a1", "calendar-state-1", "event-rsvp-remote",
                                          {.value = "Pacific/Auckland"}, {remotelyUpdated},
                                          {{.accountId = "a1",
                                            .id = "event-1",
                                            .eventId = "event-1",
                                            .recurrenceId = std::nullopt,
                                            .localStart = remotelyUpdated.start,
                                            .localEnd = {.value = "2026-07-13T10:00:00"},
                                            .utcStart = std::nullopt,
                                            .utcEnd = std::nullopt,
                                            .allDay = false}},
                                          {});
            REQUIRE_FALSE(cacheError.has_value());
        };

        const auto result = QCoro::waitFor(mutation.respond(settings, "a1",
                                                            {.accountId = "a1",
                                                             .eventId = "event-1",
                                                             .participationStatus = "accepted",
                                                             .ifInState = std::nullopt,
                                                             .materialization = std::nullopt}));

        REQUIRE(std::holds_alternative<javelin::jmap::calendar::CommittedMutation>(result));
        REQUIRE(transport.requests.size() == 2);
        const auto& arguments = transport.requests[1].envelope.methodCalls.front().arguments;
        CHECK(arguments.find(R"("ifInState":"event-rsvp-remote")") != std::string::npos);
        CHECK(arguments.find(R"("participants/me/participationStatus":"accepted")") !=
              std::string::npos);
        CHECK(arguments.find("Organizer changed the title") == std::string::npos);
        const auto cached = calendars.findEvent("a1", "event-1");
        REQUIRE(
            std::holds_alternative<std::optional<javelin::jmap::calendar::CalendarEvent>>(cached));
        REQUIRE(
            std::get<std::optional<javelin::jmap::calendar::CalendarEvent>>(cached).has_value());
        CHECK(std::get<std::optional<javelin::jmap::calendar::CalendarEvent>>(cached)->title ==
              "Organizer changed the title");
    }

    SECTION("a participant identity must match the invitation")
    {
        transport.results.push_back(identityResponse("mailto:other@example.test"));

        const auto result = QCoro::waitFor(mutation.respond(settings, "a1",
                                                            {.accountId = "a1",
                                                             .eventId = "event-1",
                                                             .participationStatus = "accepted",
                                                             .ifInState = std::nullopt,
                                                             .materialization = std::nullopt}));

        REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
        CHECK(std::get<javelin::jmap::OperationError>(result).code ==
              javelin::jmap::OperationErrorCode::InvalidRequest);
        REQUIRE(transport.requests.size() == 1);
    }

    SECTION("mayRSVP is required before querying participant identities")
    {
        auto noRsvp = work;
        noRsvp.myRights.mayRSVP = false;
        REQUIRE_FALSE(calendars.replaceCalendars("a1", "calendar-state-2", {noRsvp}).has_value());

        const auto result = QCoro::waitFor(mutation.respond(settings, "a1",
                                                            {.accountId = "a1",
                                                             .eventId = "event-1",
                                                             .participationStatus = "accepted",
                                                             .ifInState = std::nullopt,
                                                             .materialization = std::nullopt}));

        REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
        CHECK(std::get<javelin::jmap::OperationError>(result).code ==
              javelin::jmap::OperationErrorCode::PermissionDenied);
        CHECK(transport.requests.empty());
    }

    SECTION("non-origin fields cannot be edited but private settings use mayUpdatePrivate")
    {
        auto renamed = invitation;
        renamed.title = "Not my meeting";
        const auto forbidden = QCoro::waitFor(mutation.update(settings, "a1",
                                                              {.accountId = "a1",
                                                               .event = renamed,
                                                               .operationGroupId = std::nullopt,
                                                               .ifInState = std::nullopt,
                                                               .materialization = std::nullopt}));
        REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(forbidden));
        CHECK(std::get<javelin::jmap::OperationError>(forbidden).code ==
              javelin::jmap::OperationErrorCode::PermissionDenied);
        CHECK(transport.requests.empty());

        transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {{.name = "CalendarEvent/set",
                  .arguments =
                      R"({"accountId":"a1","oldState":"event-rsvp-1","newState":"event-rsvp-2","created":{},"updated":{"event-1":null},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{}})",
                  .callId = "calendar-event-set"}},
            .createdIds = std::nullopt,
            .sessionState = "session-private-update",
        });
        auto privateUpdate = invitation;
        privateUpdate.useDefaultAlerts = true;
        const auto allowed = QCoro::waitFor(mutation.update(settings, "a1",
                                                            {.accountId = "a1",
                                                             .event = std::move(privateUpdate),
                                                             .operationGroupId = std::nullopt,
                                                             .ifInState = std::nullopt,
                                                             .materialization = std::nullopt}));
        REQUIRE(std::holds_alternative<javelin::jmap::calendar::CommittedMutation>(allowed));
        REQUIRE(transport.requests.size() == 1);
        CHECK(transport.requests.front().envelope.methodCalls.front().arguments.find(
                  R"("useDefaultAlerts":true)") != std::string::npos);
    }

    SECTION("a non-origin event owned by the configured identity remains editable")
    {
        auto owned = invitation;
        owned.attendees.front().email = "alice@example.test";
        owned.attendees.front().calendarAddress = "mailto:alice@example.test";
        owned.attendees[1].email = "guest@example.test";
        owned.attendees[1].calendarAddress = "mailto:guest@example.test";
        REQUIRE_FALSE(
            calendars
                .reconcileWindow({.accountId = "a1",
                                  .start = {.value = "2026-06-29T00:00:00"},
                                  .end = {.value = "2026-08-10T00:00:00"},
                                  .displayTimeZone = {.value = "Pacific/Auckland"},
                                  .queryState = "query-owned",
                                  .eventState = "event-rsvp-1",
                                  .events = {owned},
                                  .occurrences = {{.accountId = "a1",
                                                   .id = "event-1",
                                                   .eventId = "event-1",
                                                   .recurrenceId = std::nullopt,
                                                   .localStart = owned.start,
                                                   .localEnd = {.value = "2026-07-13T10:00:00"},
                                                   .utcStart = std::nullopt,
                                                   .utcEnd = std::nullopt,
                                                   .allDay = false}}})
                .has_value());
        auto writable = work;
        writable.myRights.mayWriteOwn = true;
        REQUIRE_FALSE(
            calendars.replaceCalendars("a1", "calendar-state-owned", {writable}).has_value());
        transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {{.name = "CalendarEvent/set",
                  .arguments =
                      R"({"accountId":"a1","oldState":"event-rsvp-1","newState":"event-owned-2","created":{},"updated":{"event-1":null},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{}})",
                  .callId = "calendar-event-set"}},
            .createdIds = std::nullopt,
            .sessionState = "session-owned-update",
        });
        owned.title = "My renamed meeting";

        const auto result = QCoro::waitFor(mutation.update(settings, "a1",
                                                           {.accountId = "a1",
                                                            .event = std::move(owned),
                                                            .operationGroupId = std::nullopt,
                                                            .ifInState = std::nullopt,
                                                            .materialization = std::nullopt}));

        REQUIRE(std::holds_alternative<javelin::jmap::calendar::CommittedMutation>(result));
        REQUIRE(transport.requests.size() == 1);
        CHECK(transport.requests.front().envelope.methodCalls.front().arguments.find(
                  R"("title":"My renamed meeting")") != std::string::npos);
    }

    SECTION("a non-origin ownerless event remains editable with mayWriteOwn")
    {
        auto ownerless = invitation;
        ownerless.attendees.clear();
        ownerless.organizerCalendarAddress.reset();
        REQUIRE_FALSE(
            calendars
                .reconcileWindow({.accountId = "a1",
                                  .start = {.value = "2026-06-29T00:00:00"},
                                  .end = {.value = "2026-08-10T00:00:00"},
                                  .displayTimeZone = {.value = "Pacific/Auckland"},
                                  .queryState = "query-ownerless",
                                  .eventState = "event-rsvp-1",
                                  .events = {ownerless},
                                  .occurrences = {{.accountId = "a1",
                                                   .id = "event-1",
                                                   .eventId = "event-1",
                                                   .recurrenceId = std::nullopt,
                                                   .localStart = ownerless.start,
                                                   .localEnd = {.value = "2026-07-13T10:00:00"},
                                                   .utcStart = std::nullopt,
                                                   .utcEnd = std::nullopt,
                                                   .allDay = false}}})
                .has_value());
        auto writable = work;
        writable.myRights.mayWriteOwn = true;
        REQUIRE_FALSE(
            calendars.replaceCalendars("a1", "calendar-state-ownerless", {writable}).has_value());
        transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {{.name = "CalendarEvent/set",
                  .arguments =
                      R"({"accountId":"a1","oldState":"event-rsvp-1","newState":"event-ownerless-2","created":{},"updated":{"event-1":null},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{}})",
                  .callId = "calendar-event-set"}},
            .createdIds = std::nullopt,
            .sessionState = "session-ownerless-update",
        });
        ownerless.title = "Renamed ownerless event";

        const auto result = QCoro::waitFor(mutation.update(settings, "a1",
                                                           {.accountId = "a1",
                                                            .event = std::move(ownerless),
                                                            .operationGroupId = std::nullopt,
                                                            .ifInState = std::nullopt,
                                                            .materialization = std::nullopt}));

        REQUIRE(std::holds_alternative<javelin::jmap::calendar::CommittedMutation>(result));
        REQUIRE(transport.requests.size() == 1);
    }
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

    javelin::jmap::calendar::CalendarCacheReader reader{connection};
    javelin::jmap::calendar::CalendarProtocolClient protocol{connection, transport};
    javelin::jmap::calendar::CalendarSyncEngine sync{connection, protocol};
    javelin::jmap::calendar::CalendarMutationEngine mutation{connection, protocol, sync, reader};
    const javelin::jmap::calendar::VisibleInterval interval{
        .start = {.value = "2026-06-29T00:00:00"}, .end = {.value = "2026-08-10T00:00:00"}};
    const javelin::jmap::calendar::TimeZoneId zone{.value = "Pacific/Auckland"};
    const auto refreshed =
        QCoro::waitFor(sync.refresh({.sessionUrl = "https://example.test/.well-known/jmap",
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
