#include "app/CalendarInvitationService.h"

#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/Session.h"
#include "jmap/cache/CalendarRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/calendar/CalendarCacheReader.h"
#include "jmap/calendar/CalendarProtocolClient.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QEventLoop>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    void ensureApplication()
    {
        if (QCoreApplication::instance() != nullptr)
            return;
        static int argc = 1;
        static char name[] = "calendar-invitation-service-test";
        static char* argv[] = {name, nullptr};
        static QCoreApplication application(argc, argv);
        Q_UNUSED(application);
    }

    class FakeAccountSource final : public javelin::app::CalendarInvitationAccountSource
    {
      public:
        javelin::app::AccountConnectionSettings settings{
            .connectionId = "connection-1",
            .revision = 1,
            .sessionUrl = "https://example.test/.well-known/jmap",
            .loginEmail = "alice@example.test",
            .apiKey = "secret",
            .refreshToken = {},
            .tokenEndpoint = {},
            .oauthClientId = {},
        };

        [[nodiscard]] std::optional<javelin::app::AccountConnectionSettings>
        connectionSettingsFor(const std::string_view ownerAccountId) const override
        {
            return ownerAccountId == "owner" ? std::optional{settings} : std::nullopt;
        }

        [[nodiscard]] std::vector<std::string> configuredAccountIds() const override
        {
            return {"owner"};
        }
    };

    class FakeMethodTransport final : public javelin::jmap::api::JmapMethodTransport
    {
      public:
        std::vector<javelin::jmap::api::JmapMethodRequest> requests;
        std::vector<javelin::jmap::api::JmapMethodTransportResult> results;

        QCoro::Task<javelin::jmap::api::JmapMethodTransportResult>
        call(javelin::jmap::api::JmapMethodRequest request) override
        {
            requests.push_back(std::move(request));
            REQUIRE_FALSE(results.empty());
            auto result = std::move(results.front());
            results.erase(results.begin());
            co_return result;
        }
    };

    javelin::jmap::api::ResponseEnvelope response(std::string name, std::string arguments,
                                                  std::string callId)
    {
        return {.methodResponses = {{.name = std::move(name),
                                     .arguments = std::move(arguments),
                                     .callId = std::move(callId)}},
                .createdIds = std::nullopt,
                .sessionState = "session-2"};
    }

    javelin::jmap::api::Session calendarSession()
    {
        javelin::jmap::api::Session value;
        value.username = "alice@example.test";
        value.apiUrl = "https://example.test/jmap";
        value.state = "session-1";
        value.capabilities.core = true;
        value.capabilities.coreDetails = javelin::jmap::api::CoreCapability{};
        value.capabilities.coreDetails->maxObjectsInGet = 256;
        value.capabilities.calendars = true;
        value.primaryAccounts.calendarsAccountId = "calendar-account";
        value.accounts.emplace("owner", javelin::jmap::api::Account{.id = "owner",
                                                                    .name = "Owner",
                                                                    .isPersonal = true,
                                                                    .isReadOnly = false,
                                                                    .accountCapabilities = {}});
        value.accounts.emplace(
            "calendar-account",
            javelin::jmap::api::Account{
                .id = "calendar-account",
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

    void seedCalendarMetadata(javelin::jmap::cache::DatabaseConnection& connection)
    {
        javelin::jmap::cache::CalendarRepository repository{connection};
        REQUIRE_FALSE(repository
                          .replaceCalendars("calendar-account", "c1",
                                            {javelin::jmap::calendar::Calendar{
                                                .accountId = "calendar-account",
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
                                                .myRights = {.mayReadItems = true, .mayRSVP = true},
                                            }})
                          .has_value());
    }

    struct InvitationSignal
    {
        QString key;
        QString accountId;
        QString eventId;
        QString recurrenceId;
        QString navigationDate;
        QString title;
    };

    std::optional<InvitationSignal>
    runUntilInvitation(javelin::app::CalendarInvitationService& service)
    {
        QEventLoop loop;
        InvitationSignal signal;
        bool delivered = false;
        QObject::connect(&service, &javelin::app::CalendarInvitationService::invitationReady, &loop,
                         [&signal, &delivered, &loop](
                             const QString& key, const QString& accountId, const QString& eventId,
                             const QString& recurrenceId, const QString& navigationDate,
                             const QString& title, const QString&)
                         {
                             signal = {.key = key,
                                       .accountId = accountId,
                                       .eventId = eventId,
                                       .recurrenceId = recurrenceId,
                                       .navigationDate = navigationDate,
                                       .title = title};
                             delivered = true;
                             loop.quit();
                         });
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(3000);
        service.start();
        service.accountChanged(QStringLiteral("owner"));
        loop.exec();
        return delivered ? std::optional{signal} : std::nullopt;
    }

    bool runUntilCacheChanged(javelin::app::CalendarInvitationService& service,
                              const bool triggerSynchronization = true)
    {
        service.start();
        QEventLoop loop;
        bool changed = false;
        QObject::connect(&service,
                         &javelin::app::CalendarInvitationService::pendingInvitationCacheChanged,
                         &loop,
                         [&changed, &loop]
                         {
                             changed = true;
                             loop.quit();
                         });
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(3000);
        if (triggerSynchronization)
            service.accountChanged(QStringLiteral("owner"));
        loop.exec();
        return changed;
    }
} // namespace

TEST_CASE("calendar invitation service startup does not independently synchronize accounts",
          "[app][calendar][invitation][service][startup]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-invitation-service-startup"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));

    FakeMethodTransport transport;
    javelin::jmap::calendar::CalendarCacheReader reader{connection};
    javelin::jmap::calendar::CalendarProtocolClient protocol{connection, transport};
    FakeAccountSource accounts;
    javelin::app::CalendarInvitationService service{connection, protocol, reader, accounts};

    service.start();
    QCoreApplication::processEvents();
    CHECK(transport.requests.empty());
}

TEST_CASE("calendar invitation service discovers an occurrence-only invitation",
          "[app][calendar][invitation][service]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-invitation-service-occurrence"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("owner", calendarSession()).has_value());
    seedCalendarMetadata(connection);

    FakeMethodTransport transport;
    transport.results = {
        response(
            "ParticipantIdentity/get",
            R"({"accountId":"calendar-account","state":"p1","list":[{"id":"alias","name":"Alice Alias","calendarAddress":"mailto:alias@example.test","isDefault":false}],"notFound":[]})",
            "calendar-invitation-identities"),
        response(
            "CalendarEventNotification/query",
            R"({"accountId":"calendar-account","queryState":"nq1","canCalculateChanges":false,"position":0,"ids":["notification-1"],"total":1})",
            "calendar-invitation-query"),
        response(
            "CalendarEventNotification/get",
            R"({"accountId":"calendar-account","state":"n1","list":[{"id":"notification-1","created":"2026-08-15T00:00:00Z","changedBy":{"name":"Organizer"},"type":"created","calendarEventId":"series-1","isDraft":false,"event":{"@type":"Event","id":"instance-1","baseEventId":"series-1","recurrenceId":"2026-09-08T09:00:00","uid":"series-uid","calendarIds":{"work":true},"title":"Occurrence invitation","start":"2026-09-08T09:00:00","duration":"PT1H","timeZone":"Pacific/Auckland","isDraft":false,"isOrigin":false,"participants":{"alias":{"@type":"Participant","name":"Alice Alias","calendarAddress":"mailto:alias@example.test","participationStatus":"needs-action","roles":{"attendee":true}}}}}],"notFound":[]})",
            "calendar-invitation-notification-get"),
        response(
            "CalendarEvent/query",
            R"({"accountId":"calendar-account","queryState":"eq1","canCalculateChanges":false,"position":0,"ids":["series-1"],"total":1})",
            "calendar-invitation-query"),
        response(
            "CalendarEvent/get",
            R"({"accountId":"calendar-account","state":"e1","list":[{"@type":"Event","id":"series-1","uid":"series-uid","calendarIds":{"work":true},"title":"Recurring meeting","start":"2026-09-01T09:00:00","duration":"PT1H","timeZone":"Pacific/Auckland","isDraft":false,"isOrigin":false,"recurrenceRule":{"@type":"RecurrenceRule","frequency":"weekly"},"recurrenceOverrides":{"2026-09-08T09:00:00":{"participants/alias":{"@type":"Participant","name":"Alice Alias","calendarAddress":"mailto:alias@example.test","participationStatus":"needs-action","roles":{"attendee":true}}}},"participants":{"organizer":{"@type":"Participant","name":"Organizer","calendarAddress":"mailto:organizer@example.test","participationStatus":"accepted","roles":{"owner":true,"attendee":true}}}}],"notFound":[]})",
            "calendar-invitation-event-get"),
    };

    javelin::jmap::calendar::CalendarCacheReader reader{connection};
    javelin::jmap::calendar::CalendarProtocolClient protocol{connection, transport};
    FakeAccountSource accounts;
    javelin::app::CalendarInvitationService service{connection, protocol, reader, accounts};

    const auto signal = runUntilInvitation(service);
    REQUIRE(signal.has_value());
    CHECK(signal->accountId == QStringLiteral("calendar-account"));
    CHECK(signal->eventId == QStringLiteral("series-1"));
    CHECK(signal->recurrenceId == QStringLiteral("2026-09-08T09:00:00"));
    CHECK(signal->navigationDate == QStringLiteral("2026-09-08"));
    CHECK(signal->title == QStringLiteral("Recurring meeting"));

    const auto pending = reader.pendingInvitations();
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::calendar::PendingCalendarInvitation>>(
        pending));
    const auto& invitations =
        std::get<std::vector<javelin::jmap::calendar::PendingCalendarInvitation>>(pending);
    REQUIRE(invitations.size() == 1);
    REQUIRE(invitations.front().recurrenceId.has_value());
    CHECK(invitations.front().recurrenceId->value == "2026-09-08T09:00:00");
    CHECK(invitations.front().selfParticipantId == "alias");
    CHECK(invitations.front().participationStatus == "needs-action");
    CHECK(transport.results.empty());
}

TEST_CASE("calendar invitation full reconciliation alerts for an unseen created notification",
          "[app][calendar][invitation][service][reconciliation]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-invitation-service-reconciliation"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("owner", calendarSession()).has_value());
    seedCalendarMetadata(connection);
    QSqlQuery seed{connection.database()};
    REQUIRE(seed.exec(
        QStringLiteral("INSERT INTO calendar_state_tokens(account_id,data_type,state) VALUES "
                       "('calendar-account','CalendarEventNotification','n0')")));
    REQUIRE(seed.exec(QStringLiteral(
        "INSERT INTO calendar_event_notifications(account_id,notification_id,type,is_deleted) "
        "VALUES('calendar-account','notification-old','created',0)")));

    FakeMethodTransport transport;
    transport.results = {
        response(
            "ParticipantIdentity/get",
            R"({"accountId":"calendar-account","state":"p1","list":[{"id":"self","name":"Alice","calendarAddress":"mailto:alice@example.test","isDefault":true}],"notFound":[]})",
            "calendar-invitation-identities"),
        response("error", R"({"type":"cannotCalculateChanges","description":"state too old"})",
                 "calendar-invitation-notification-changes"),
        response(
            "CalendarEventNotification/query",
            R"({"accountId":"calendar-account","queryState":"nq2","canCalculateChanges":false,"position":0,"ids":["notification-new"],"total":1})",
            "calendar-invitation-query"),
        response(
            "CalendarEventNotification/get",
            R"({"accountId":"calendar-account","state":"n2","list":[{"id":"notification-new","created":"2026-08-15T03:00:00Z","changedBy":{"name":"Organizer"},"type":"created","calendarEventId":"event-new","isDraft":false,"event":{"@type":"Event","id":"event-new","uid":"event-new-uid","calendarIds":{"work":true},"title":"Recovered invitation","start":"2026-09-01T09:00:00","duration":"PT1H","timeZone":"Pacific/Auckland","isDraft":false,"isOrigin":false,"participants":{"self":{"@type":"Participant","name":"Alice","calendarAddress":"mailto:alice@example.test","participationStatus":"needs-action","roles":{"attendee":true}}}}}],"notFound":[]})",
            "calendar-invitation-notification-get"),
        response(
            "CalendarEvent/get",
            R"({"accountId":"calendar-account","state":"e2","list":[{"@type":"Event","id":"event-new","uid":"event-new-uid","calendarIds":{"work":true},"title":"Recovered invitation","start":"2026-09-01T09:00:00","duration":"PT1H","timeZone":"Pacific/Auckland","isDraft":false,"isOrigin":false,"participants":{"organizer":{"@type":"Participant","name":"Organizer","calendarAddress":"mailto:organizer@example.test","participationStatus":"accepted","roles":{"owner":true,"attendee":true}},"self":{"@type":"Participant","name":"Alice","calendarAddress":"mailto:alice@example.test","participationStatus":"needs-action","roles":{"attendee":true}}}}],"notFound":[]})",
            "calendar-invitation-event-get"),
    };

    javelin::jmap::calendar::CalendarCacheReader reader{connection};
    javelin::jmap::calendar::CalendarProtocolClient protocol{connection, transport};
    FakeAccountSource accounts;
    javelin::app::CalendarInvitationService service{connection, protocol, reader, accounts};

    const auto signal = runUntilInvitation(service);
    REQUIRE(signal.has_value());
    CHECK(signal->eventId == QStringLiteral("event-new"));
    CHECK(signal->title == QStringLiteral("Recovered invitation"));
    CHECK(transport.results.empty());
}

TEST_CASE("calendar invitation service resolves the next recurring occurrence for presentation",
          "[app][calendar][invitation][service][recurrence]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-invitation-service-next-occurrence"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("owner", calendarSession()).has_value());
    seedCalendarMetadata(connection);

    FakeMethodTransport transport;
    transport.results = {
        response(
            "ParticipantIdentity/get",
            R"({"accountId":"calendar-account","state":"p1","list":[{"id":"self","name":"Alice","calendarAddress":"mailto:alice@example.test","isDefault":true}],"notFound":[]})",
            "calendar-invitation-identities"),
        response(
            "CalendarEventNotification/query",
            R"({"accountId":"calendar-account","queryState":"nq1","canCalculateChanges":false,"position":0,"ids":["notification-1"],"total":1})",
            "calendar-invitation-query"),
        response(
            "CalendarEventNotification/get",
            R"({"accountId":"calendar-account","state":"n1","list":[{"id":"notification-1","created":"2026-08-15T00:00:00Z","changedBy":{"name":"Organizer"},"type":"created","calendarEventId":"series-1","isDraft":false,"event":{"@type":"Event","id":"series-1","uid":"series-uid","calendarIds":{"work":true},"title":"Long-running meeting","start":"2026-01-01T09:00:00","duration":"PT1H","timeZone":"Pacific/Auckland","isDraft":false,"isOrigin":false,"recurrenceRule":{"@type":"RecurrenceRule","frequency":"weekly"},"participants":{"self":{"@type":"Participant","name":"Alice","calendarAddress":"mailto:alice@example.test","participationStatus":"needs-action","roles":{"attendee":true}}}}}],"notFound":[]})",
            "calendar-invitation-notification-get"),
        response(
            "CalendarEvent/query",
            R"({"accountId":"calendar-account","queryState":"eq1","canCalculateChanges":false,"position":0,"ids":["series-1"],"total":1})",
            "calendar-invitation-query"),
        response(
            "CalendarEvent/get",
            R"({"accountId":"calendar-account","state":"e1","list":[{"@type":"Event","id":"series-1","uid":"series-uid","calendarIds":{"work":true},"title":"Long-running meeting","start":"2026-01-01T09:00:00","duration":"PT1H","timeZone":"Pacific/Auckland","isDraft":false,"isOrigin":false,"recurrenceRule":{"@type":"RecurrenceRule","frequency":"weekly"},"participants":{"self":{"@type":"Participant","name":"Alice","calendarAddress":"mailto:alice@example.test","participationStatus":"needs-action","roles":{"attendee":true}}}}],"notFound":[]})",
            "calendar-invitation-event-get"),
        response(
            "CalendarEvent/query",
            R"({"accountId":"calendar-account","queryState":"expanded-q1","canCalculateChanges":false,"position":0,"ids":["instance-next"]})",
            "calendar-invitation-next-occurrence"),
        response(
            "CalendarEvent/get",
            R"({"accountId":"calendar-account","state":"e1","list":[{"@type":"Event","id":"instance-next","baseEventId":"series-1","recurrenceId":"2026-09-03T09:00:00","uid":"series-uid","calendarIds":{"work":true},"title":"Long-running meeting","start":"2026-09-03T09:00:00","duration":"PT1H","timeZone":"Pacific/Auckland","isDraft":false,"isOrigin":false,"participants":{"self":{"@type":"Participant","name":"Alice","calendarAddress":"mailto:alice@example.test","participationStatus":"needs-action","roles":{"attendee":true}}}}],"notFound":[]})",
            "calendar-invitation-event-get"),
    };

    javelin::jmap::calendar::CalendarCacheReader reader{connection};
    javelin::jmap::calendar::CalendarProtocolClient protocol{connection, transport};
    FakeAccountSource accounts;
    javelin::app::CalendarInvitationService service{connection, protocol, reader, accounts};

    const auto signal = runUntilInvitation(service);
    QSqlQuery storedPending{connection.database()};
    REQUIRE(storedPending.exec(QStringLiteral(
        "SELECT count(*) FROM calendar_pending_invitations WHERE account_id='calendar-account'")));
    REQUIRE(storedPending.next());
    CHECK(storedPending.value(0).toInt() == 1);
    const auto pending = reader.pendingInvitations();
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::calendar::PendingCalendarInvitation>>(
        pending));
    const auto& invitations =
        std::get<std::vector<javelin::jmap::calendar::PendingCalendarInvitation>>(pending);
    REQUIRE(invitations.size() == 1);
    const auto& invitation = invitations.front();
    REQUIRE(signal.has_value());
    CHECK(signal->eventId == QStringLiteral("series-1"));
    CHECK(signal->recurrenceId == QStringLiteral("2026-09-03T09:00:00"));
    CHECK(signal->navigationDate == QStringLiteral("2026-09-03"));
    CHECK_FALSE(invitation.recurrenceId.has_value());
    REQUIRE(invitation.displayRecurrenceId.has_value());
    CHECK(invitation.displayRecurrenceId->value == "2026-09-03T09:00:00");
    CHECK(invitation.displayTime.value == "2026-09-03T09:00:00");
    CHECK(transport.results.empty());
}

TEST_CASE("calendar invitation missing or empty baseline keeps authoritative state tokens",
          "[app][calendar][invitation][service][reconciliation]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-invitation-empty-baseline"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("owner", calendarSession()).has_value());
    seedCalendarMetadata(connection);

    SECTION("missing notification state")
    {
    }
    SECTION("poisoned empty notification state")
    {
        QSqlQuery seed{connection.database()};
        REQUIRE(seed.exec(
            QStringLiteral("INSERT INTO calendar_state_tokens(account_id,data_type,state) VALUES "
                           "('calendar-account','CalendarEventNotification','')")));
    }

    FakeMethodTransport transport;
    transport.results = {
        response(
            "ParticipantIdentity/get",
            R"({"accountId":"calendar-account","state":"p1","list":[{"id":"self","name":"Alice","calendarAddress":"mailto:alice@example.test","isDefault":true}],"notFound":[]})",
            "calendar-invitation-identities"),
        response(
            "CalendarEventNotification/query",
            R"({"accountId":"calendar-account","queryState":"nq1","canCalculateChanges":false,"position":0,"ids":[],"total":0})",
            "calendar-invitation-query"),
        response("CalendarEventNotification/get",
                 R"({"accountId":"calendar-account","state":"n1","list":[],"notFound":[]})",
                 "calendar-invitation-notification-get"),
        response(
            "CalendarEvent/query",
            R"({"accountId":"calendar-account","queryState":"eq1","canCalculateChanges":false,"position":0,"ids":[],"total":0})",
            "calendar-invitation-query"),
        response("CalendarEvent/get",
                 R"({"accountId":"calendar-account","state":"e1","list":[],"notFound":[]})",
                 "calendar-invitation-event-get"),
    };

    javelin::jmap::calendar::CalendarCacheReader reader{connection};
    javelin::jmap::calendar::CalendarProtocolClient protocol{connection, transport};
    FakeAccountSource accounts;
    javelin::app::CalendarInvitationService service{connection, protocol, reader, accounts};

    REQUIRE(runUntilCacheChanged(service));
    CHECK(transport.results.empty());
    CHECK(transport.requests.size() == 5);
    REQUIRE(transport.requests.at(1).envelope.methodCalls.size() == 1);
    CHECK(transport.requests.at(1).envelope.methodCalls.front().name ==
          "CalendarEventNotification/query");

    javelin::jmap::cache::CalendarRepository repository{connection};
    const auto notificationState =
        repository.stateToken("calendar-account", "CalendarEventNotification");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(notificationState));
    REQUIRE(std::get<std::optional<std::string>>(notificationState).has_value());
    CHECK(*std::get<std::optional<std::string>>(notificationState) == "n1");
}

TEST_CASE("calendar invitation no-op changes do not reschedule forever",
          "[app][calendar][invitation][service][reconciliation]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-invitation-empty-incremental"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("owner", calendarSession()).has_value());
    seedCalendarMetadata(connection);
    QSqlQuery seed{connection.database()};
    REQUIRE(seed.exec(
        QStringLiteral("INSERT INTO calendar_state_tokens(account_id,data_type,state) VALUES "
                       "('calendar-account','CalendarEventNotification','n0')")));

    FakeMethodTransport transport;
    transport.results = {
        response(
            "ParticipantIdentity/get",
            R"({"accountId":"calendar-account","state":"p1","list":[{"id":"self","name":"Alice","calendarAddress":"mailto:alice@example.test","isDefault":true}],"notFound":[]})",
            "calendar-invitation-identities"),
        response(
            "CalendarEventNotification/changes",
            R"({"accountId":"calendar-account","oldState":"n0","newState":"n1","hasMoreChanges":false,"created":[],"updated":[],"destroyed":[]})",
            "calendar-invitation-notification-changes"),
        response("CalendarEventNotification/get",
                 R"({"accountId":"calendar-account","state":"n1","list":[],"notFound":[]})",
                 "calendar-invitation-notification-get"),
        response("CalendarEvent/get",
                 R"({"accountId":"calendar-account","state":"e1","list":[],"notFound":[]})",
                 "calendar-invitation-event-get"),
    };

    javelin::jmap::calendar::CalendarCacheReader reader{connection};
    javelin::jmap::calendar::CalendarProtocolClient protocol{connection, transport};
    FakeAccountSource accounts;
    javelin::app::CalendarInvitationService service{connection, protocol, reader, accounts};

    REQUIRE(runUntilCacheChanged(service));
    CHECK(transport.results.empty());
    REQUIRE(transport.requests.size() == 4);

    QEventLoop settle;
    QTimer::singleShot(400, &settle, &QEventLoop::quit);
    settle.exec();
    CHECK(transport.requests.size() == 4);

    javelin::jmap::cache::CalendarRepository repository{connection};
    const auto notificationState =
        repository.stateToken("calendar-account", "CalendarEventNotification");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(notificationState));
    REQUIRE(std::get<std::optional<std::string>>(notificationState).has_value());
    CHECK(*std::get<std::optional<std::string>>(notificationState) == "n1");
}

TEST_CASE("calendar invitation changes fall back when pagination state does not advance",
          "[app][calendar][invitation][service][reconciliation][pagination]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-invitation-nonadvancing-changes"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("owner", calendarSession()).has_value());
    seedCalendarMetadata(connection);
    QSqlQuery seed{connection.database()};
    REQUIRE(seed.exec(
        QStringLiteral("INSERT INTO calendar_state_tokens(account_id,data_type,state) VALUES "
                       "('calendar-account','CalendarEventNotification','n0')")));

    FakeMethodTransport transport;
    transport.results = {
        response(
            "ParticipantIdentity/get",
            R"({"accountId":"calendar-account","state":"p1","list":[{"id":"self","name":"Alice","calendarAddress":"mailto:alice@example.test","isDefault":true}],"notFound":[]})",
            "calendar-invitation-identities"),
        response(
            "CalendarEventNotification/changes",
            R"({"accountId":"calendar-account","oldState":"n0","newState":"n0","hasMoreChanges":true,"created":[],"updated":[],"destroyed":[]})",
            "calendar-invitation-notification-changes"),
        response(
            "CalendarEventNotification/query",
            R"({"accountId":"calendar-account","queryState":"nq1","canCalculateChanges":false,"position":0,"ids":[],"total":0})",
            "calendar-invitation-query"),
        response("CalendarEventNotification/get",
                 R"({"accountId":"calendar-account","state":"n1","list":[],"notFound":[]})",
                 "calendar-invitation-notification-get"),
        response("CalendarEvent/get",
                 R"({"accountId":"calendar-account","state":"e1","list":[],"notFound":[]})",
                 "calendar-invitation-event-get"),
    };

    javelin::jmap::calendar::CalendarCacheReader reader{connection};
    javelin::jmap::calendar::CalendarProtocolClient protocol{connection, transport};
    FakeAccountSource accounts;
    javelin::app::CalendarInvitationService service{connection, protocol, reader, accounts};

    REQUIRE(runUntilCacheChanged(service));
    CHECK(transport.results.empty());
    REQUIRE(transport.requests.size() == 5);
    CHECK(transport.requests.at(1).envelope.methodCalls.front().name ==
          "CalendarEventNotification/changes");
    CHECK(transport.requests.at(2).envelope.methodCalls.front().name ==
          "CalendarEventNotification/query");

    javelin::jmap::cache::CalendarRepository repository{connection};
    const auto notificationState =
        repository.stateToken("calendar-account", "CalendarEventNotification");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(notificationState));
    REQUIRE(std::get<std::optional<std::string>>(notificationState).has_value());
    CHECK(*std::get<std::optional<std::string>>(notificationState) == "n1");
}

TEST_CASE("calendar invitation changes page cap falls back to full reconciliation",
          "[app][calendar][invitation][service][reconciliation][pagination]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-invitation-changes-page-cap"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("owner", calendarSession()).has_value());
    seedCalendarMetadata(connection);
    QSqlQuery seed{connection.database()};
    REQUIRE(seed.exec(
        QStringLiteral("INSERT INTO calendar_state_tokens(account_id,data_type,state) VALUES "
                       "('calendar-account','CalendarEventNotification','n0')")));

    FakeMethodTransport transport;
    transport.results.push_back(response(
        "ParticipantIdentity/get",
        R"({"accountId":"calendar-account","state":"p1","list":[{"id":"self","name":"Alice","calendarAddress":"mailto:alice@example.test","isDefault":true}],"notFound":[]})",
        "calendar-invitation-identities"));
    for (std::size_t page = 0; page < javelin::app::calendarInvitationChangesPageLimit; ++page)
    {
        const auto oldState = std::string{"n"} + std::to_string(page);
        const auto newState = std::string{"n"} + std::to_string(page + 1);
        transport.results.push_back(response(
            "CalendarEventNotification/changes",
            std::string{R"({"accountId":"calendar-account","oldState":")"} + oldState +
                R"(","newState":")" + newState +
                R"(","hasMoreChanges":true,"created":[],"updated":[],"destroyed":[]})",
            "calendar-invitation-notification-changes"));
    }
    transport.results.push_back(response(
        "CalendarEventNotification/query",
        R"({"accountId":"calendar-account","queryState":"nq-full","canCalculateChanges":false,"position":0,"ids":[],"total":0})",
        "calendar-invitation-query"));
    transport.results.push_back(response(
        "CalendarEventNotification/get",
        R"({"accountId":"calendar-account","state":"n-full","list":[],"notFound":[]})",
        "calendar-invitation-notification-get"));
    transport.results.push_back(response(
        "CalendarEvent/get",
        R"({"accountId":"calendar-account","state":"e1","list":[],"notFound":[]})",
        "calendar-invitation-event-get"));

    javelin::jmap::calendar::CalendarCacheReader reader{connection};
    javelin::jmap::calendar::CalendarProtocolClient protocol{connection, transport};
    FakeAccountSource accounts;
    javelin::app::CalendarInvitationService service{connection, protocol, reader, accounts};

    REQUIRE(runUntilCacheChanged(service));
    CHECK(transport.results.empty());
    REQUIRE(transport.requests.size() ==
            javelin::app::calendarInvitationChangesPageLimit + 4);
    CHECK(transport.requests
              .at(static_cast<qsizetype>(javelin::app::calendarInvitationChangesPageLimit + 1))
              .envelope.methodCalls.front().name == "CalendarEventNotification/query");

    javelin::jmap::cache::CalendarRepository repository{connection};
    const auto notificationState =
        repository.stateToken("calendar-account", "CalendarEventNotification");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(notificationState));
    REQUIRE(std::get<std::optional<std::string>>(notificationState).has_value());
    CHECK(*std::get<std::optional<std::string>>(notificationState) == "n-full");
}

TEST_CASE("calendar invitation state race commits progress before follow-up",
          "[app][calendar][invitation][service][reconciliation]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-invitation-state-race"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("owner", calendarSession()).has_value());
    seedCalendarMetadata(connection);
    QSqlQuery seed{connection.database()};
    REQUIRE(seed.exec(
        QStringLiteral("INSERT INTO calendar_state_tokens(account_id,data_type,state) VALUES "
                       "('calendar-account','CalendarEventNotification','n0')")));

    FakeMethodTransport transport;
    transport.results = {
        response(
            "ParticipantIdentity/get",
            R"({"accountId":"calendar-account","state":"p1","list":[{"id":"self","name":"Alice","calendarAddress":"mailto:alice@example.test","isDefault":true}],"notFound":[]})",
            "calendar-invitation-identities"),
        response(
            "CalendarEventNotification/changes",
            R"({"accountId":"calendar-account","oldState":"n0","newState":"n1","hasMoreChanges":false,"created":["notification-1"],"updated":[],"destroyed":[]})",
            "calendar-invitation-notification-changes"),
        response(
            "CalendarEventNotification/get",
            R"({"accountId":"calendar-account","state":"n2","list":[],"notFound":["notification-1"]})",
            "calendar-invitation-notification-get"),
        response("CalendarEvent/get",
                 R"({"accountId":"calendar-account","state":"e1","list":[],"notFound":[]})",
                 "calendar-invitation-event-get"),
        response(
            "CalendarEventNotification/changes",
            R"({"accountId":"calendar-account","oldState":"n1","newState":"n2","hasMoreChanges":false,"created":[],"updated":[],"destroyed":["notification-1"]})",
            "calendar-invitation-notification-changes"),
        response("CalendarEventNotification/get",
                 R"({"accountId":"calendar-account","state":"n2","list":[],"notFound":[]})",
                 "calendar-invitation-notification-get"),
        response("CalendarEvent/get",
                 R"({"accountId":"calendar-account","state":"e1","list":[],"notFound":[]})",
                 "calendar-invitation-event-get"),
    };

    javelin::jmap::calendar::CalendarCacheReader reader{connection};
    javelin::jmap::calendar::CalendarProtocolClient protocol{connection, transport};
    FakeAccountSource accounts;
    javelin::app::CalendarInvitationService service{connection, protocol, reader, accounts};

    REQUIRE(runUntilCacheChanged(service));
    javelin::jmap::cache::CalendarRepository repository{connection};
    auto notificationState = repository.stateToken("calendar-account", "CalendarEventNotification");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(notificationState));
    REQUIRE(std::get<std::optional<std::string>>(notificationState).has_value());
    CHECK(*std::get<std::optional<std::string>>(notificationState) == "n1");

    REQUIRE(runUntilCacheChanged(service, false));
    notificationState = repository.stateToken("calendar-account", "CalendarEventNotification");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(notificationState));
    REQUIRE(std::get<std::optional<std::string>>(notificationState).has_value());
    CHECK(*std::get<std::optional<std::string>>(notificationState) == "n2");
    CHECK(transport.results.empty());
    REQUIRE(transport.requests.size() == 7);

    QEventLoop settle;
    QTimer::singleShot(400, &settle, &QEventLoop::quit);
    settle.exec();
    CHECK(transport.requests.size() == 7);
}
