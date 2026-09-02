#include "app/CalendarNotificationService.h"
#include "app/undo/CalendarHistoryPort.h"

#include "jmap/cache/CalendarNotificationRepository.h"
#include "jmap/cache/CalendarRepository.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QEventLoop>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace
{
    void ensureApplication()
    {
        if (QCoreApplication::instance() != nullptr)
            return;
        static int argc = 1;
        static char appName[] = "calendar-notification-service-test";
        static char* argv[] = {appName, nullptr};
        static QCoreApplication application(argc, argv);
        Q_UNUSED(application);
    }

    class FakeCalendarEvents final : public javelin::app::undo::CalendarHistoryPort
    {
      public:
        javelin::jmap::calendar::CalendarEvent event;
        int authoritativeFetches = 0;
        std::string fetchedOwnerAccountId;
        std::string fetchedAccountId;
        std::optional<std::string> fetchedEventId;
        std::string fetchedUid;
        std::optional<javelin::jmap::OperationError> nextAuthoritativeError;
        int updates = 0;
        std::string updatedOwnerAccountId;
        std::optional<javelin::jmap::calendar::UpdateEventCommand> updatedCommand;

        [[nodiscard]] javelin::jmap::calendar::AuthoritativeCalendarEventResult
        getEffectiveCalendarEvent(std::string_view, const std::optional<std::string>&) override
        {
            return javelin::jmap::calendar::AuthoritativeCalendarEvent{
                .state = "event-state",
                .event = event,
            };
        }

        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::AuthoritativeCalendarEventResult>
        getAuthoritativeCalendarEvent(std::string ownerAccountId, std::string accountId,
                                      std::optional<std::string> eventId, std::string uid) override
        {
            ++authoritativeFetches;
            fetchedOwnerAccountId = ownerAccountId;
            fetchedAccountId = accountId;
            fetchedEventId = eventId;
            fetchedUid = uid;
            if (nextAuthoritativeError.has_value())
            {
                auto error = std::move(*nextAuthoritativeError);
                nextAuthoritativeError.reset();
                co_return error;
            }
            co_return javelin::jmap::calendar::AuthoritativeCalendarEvent{
                .state = "event-state",
                .event = event,
            };
        }

        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        createCalendarEvent(std::string, javelin::jmap::calendar::CreateEventCommand,
                            javelin::app::undo::CommandOrigin) override
        {
            co_return unsupported();
        }

        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        updateCalendarEvent(std::string ownerAccountId,
                            javelin::jmap::calendar::UpdateEventCommand command,
                            javelin::app::undo::CommandOrigin) override
        {
            ++updates;
            updatedOwnerAccountId = std::move(ownerAccountId);
            updatedCommand = std::move(command);
            co_return javelin::jmap::calendar::CommittedMutation{
                .accountId = updatedCommand->accountId,
                .newState = "event-state-2",
                .createdId = std::nullopt,
                .receipt = {},
            };
        }

        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        deleteCalendarEvent(std::string, javelin::jmap::calendar::DeleteEventCommand,
                            javelin::app::undo::CommandOrigin) override
        {
            co_return unsupported();
        }

      private:
        [[nodiscard]] static javelin::jmap::OperationError unsupported()
        {
            return {
                .code = javelin::jmap::OperationErrorCode::InvalidRequest,
                .message = QStringLiteral("Not used by this test"),
            };
        }
    };

    void seedOwnerAccount(javelin::jmap::cache::DatabaseConnection& connection)
    {
        QSqlQuery account{connection.database()};
        REQUIRE(account.exec(
            QStringLiteral("INSERT OR IGNORE INTO "
                           "accounts(account_id,email_address,session_url,is_primary) VALUES "
                           "('owner','owner@example.test','https://example.test/jmap',1)")));
    }

    void seedCalendarMetadata(javelin::jmap::cache::DatabaseConnection& connection,
                              const bool withDefaultAlert = false)
    {
        seedOwnerAccount(connection);
        QSqlQuery account{connection.database()};
        REQUIRE(account.exec(QStringLiteral(
            "INSERT INTO accounts(account_id,email_address,session_url,is_primary) VALUES "
            "('calendar-account','alice@example.test','https://example.test/jmap',0)")));
        javelin::jmap::calendar::Calendar calendar{
            .accountId = "calendar-account",
            .id = "work",
            .name = "Work",
            .description = std::nullopt,
            .color = std::nullopt,
            .sortOrder = 0,
            .isSubscribed = true,
            .isVisible = true,
            .isDefault = true,
            .timeZone = javelin::jmap::calendar::TimeZoneId{.value = "Etc/UTC"},
            .defaultAlertsWithTime = {},
            .defaultAlertsWithoutTime = {},
            .myRights = {.mayReadItems = true},
        };
        if (withDefaultAlert)
        {
            calendar.defaultAlertsWithTime.emplace(
                "alert-1", javelin::jmap::calendar::Alert{
                               .id = "alert-1",
                               .action = "display",
                               .triggerKind = javelin::jmap::calendar::AlertTriggerKind::Offset,
                               .relativeTo = "start",
                               .offset = javelin::jmap::calendar::Duration{.value = "-PT10M"},
                               .when = std::nullopt,
                               .acknowledged = std::nullopt,
                           });
        }
        javelin::jmap::cache::CalendarRepository calendars{connection};
        REQUIRE_FALSE(calendars.replaceCalendars("calendar-account", "calendar-state", {calendar})
                          .has_value());
    }

    javelin::jmap::calendar::CalendarEvent uncachedEvent()
    {
        const javelin::jmap::calendar::Alert alert{
            .id = "alert-1",
            .action = "display",
            .triggerKind = javelin::jmap::calendar::AlertTriggerKind::Offset,
            .relativeTo = "start",
            .offset = javelin::jmap::calendar::Duration{.value = "-PT10M"},
            .when = std::nullopt,
            .acknowledged = std::nullopt,
        };
        return {
            .accountId = "calendar-account",
            .id = "event-1",
            .baseEventId = std::nullopt,
            .recurrenceId = std::nullopt,
            .uid = "uid-1",
            .calendarIds = {{"work", true}},
            .title = "Uncached reminder",
            .description = std::nullopt,
            .location = std::nullopt,
            .start = {.value = "2026-09-03T10:00:00"},
            .duration = {.value = "PT30M"},
            .timeZone = javelin::jmap::calendar::TimeZoneId{.value = "Etc/UTC"},
            .showWithoutTime = false,
            .isDraft = false,
            .isOrigin = true,
            .useDefaultAlerts = false,
            .alerts = {{"alert-1", alert}},
            .utcStart = javelin::jmap::calendar::UtcInstant{.value = "2026-09-03T10:00:00Z"},
            .utcEnd = javelin::jmap::calendar::UtcInstant{.value = "2026-09-03T10:30:00Z"},
            .recurrenceRule = std::nullopt,
            .recurrenceOverrides = {},
            .attendees = {},
        };
    }
} // namespace

TEST_CASE("calendar alert push fetches an uncached event and delivers it once",
          "[app][calendar][notification][push]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-notification-push"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    seedCalendarMetadata(connection);

    FakeCalendarEvents calendarEvents;
    calendarEvents.event = uncachedEvent();
    javelin::app::CalendarNotificationService service{connection, calendarEvents};

    int deliveries = 0;
    QString deliveredKey;
    QString deliveredTitle;
    QEventLoop loop;
    QObject::connect(&service, &javelin::app::CalendarNotificationService::reminderDue, &loop,
                     [&deliveries, &deliveredKey, &deliveredTitle,
                      &loop](const QString& key, const QString& title, const QString&)
                     {
                         ++deliveries;
                         deliveredKey = key;
                         deliveredTitle = title;
                         loop.quit();
                     });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(2000);

    service.calendarAlertReceived(QStringLiteral("owner"), QStringLiteral("calendar-account"),
                                  QStringLiteral("event-1"), QStringLiteral("uid-1"), QString{},
                                  QStringLiteral("alert-1"));
    loop.exec();

    REQUIRE(deliveries == 1);
    CHECK(deliveredTitle == QStringLiteral("Uncached reminder"));
    CHECK_FALSE(deliveredKey.isEmpty());
    CHECK(calendarEvents.authoritativeFetches == 1);
    CHECK(calendarEvents.fetchedOwnerAccountId == "owner");
    CHECK(calendarEvents.fetchedAccountId == "calendar-account");
    REQUIRE(calendarEvents.fetchedEventId.has_value());
    CHECK(*calendarEvents.fetchedEventId == "event-1");
    CHECK(calendarEvents.fetchedUid == "uid-1");

    QSqlQuery cachedOccurrences{connection.database()};
    REQUIRE(cachedOccurrences.exec(QStringLiteral("SELECT COUNT(*) FROM calendar_occurrences")));
    REQUIRE(cachedOccurrences.next());
    CHECK(cachedOccurrences.value(0).toInt() == 0);

    service.deliveryAccepted(deliveredKey);
    service.calendarAlertReceived(QStringLiteral("owner"), QStringLiteral("calendar-account"),
                                  QStringLiteral("event-1"), QStringLiteral("uid-1"), QString{},
                                  QStringLiteral("alert-1"));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    CHECK(deliveries == 1);
    CHECK(calendarEvents.authoritativeFetches == 2);

    javelin::jmap::cache::CalendarNotificationRepository repository{connection};
    const auto claimedAgain =
        repository.claimPushed(deliveredKey.toStdString(), QDateTime::currentDateTimeUtc());
    REQUIRE(std::holds_alternative<javelin::jmap::cache::CalendarPushedAlertClaim>(claimedAgain));
    CHECK_FALSE(std::get<javelin::jmap::cache::CalendarPushedAlertClaim>(claimedAgain).claimed);
    CHECK(std::get<javelin::jmap::cache::CalendarPushedAlertClaim>(claimedAgain).completed);
}

TEST_CASE("dismissing an uncached pushed calendar alert acknowledges the authoritative event",
          "[app][calendar][notification][push][dismiss]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-notification-push-dismiss"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    seedCalendarMetadata(connection);

    FakeCalendarEvents calendarEvents;
    calendarEvents.event = uncachedEvent();
    javelin::app::CalendarNotificationService service{connection, calendarEvents};
    QEventLoop loop;
    QString deliveredKey;
    QObject::connect(&service, &javelin::app::CalendarNotificationService::reminderDue, &loop,
                     [&deliveredKey, &loop](const QString& key, const QString&, const QString&)
                     {
                         deliveredKey = key;
                         loop.quit();
                     });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(2000);
    service.calendarAlertReceived(QStringLiteral("owner"), QStringLiteral("calendar-account"),
                                  QStringLiteral("event-1"), QStringLiteral("uid-1"), QString{},
                                  QStringLiteral("alert-1"));
    loop.exec();
    REQUIRE_FALSE(deliveredKey.isEmpty());
    REQUIRE(calendarEvents.authoritativeFetches == 1);
    service.deliveryAccepted(deliveredKey);

    QSqlQuery cachedEvents{connection.database()};
    REQUIRE(cachedEvents.exec(QStringLiteral("SELECT COUNT(*) FROM calendar_events")));
    REQUIRE(cachedEvents.next());
    REQUIRE(cachedEvents.value(0).toInt() == 0);

    service.dismiss(deliveredKey);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    CHECK(calendarEvents.authoritativeFetches == 2);
    REQUIRE(calendarEvents.updates == 1);
    CHECK(calendarEvents.updatedOwnerAccountId == "owner");
    REQUIRE(calendarEvents.updatedCommand.has_value());
    CHECK(calendarEvents.updatedCommand->accountId == "calendar-account");
    CHECK(calendarEvents.updatedCommand->event.id == "event-1");
    const auto acknowledged = calendarEvents.updatedCommand->event.alerts.find("alert-1");
    REQUIRE(acknowledged != calendarEvents.updatedCommand->event.alerts.end());
    REQUIRE(acknowledged->second.acknowledged.has_value());
    CHECK_FALSE(acknowledged->second.acknowledged->value.empty());
}

TEST_CASE("calendar alert push survives a daemon restart during transient fetch retry",
          "[app][calendar][notification][push][restart]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-notification-push-restart"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    seedCalendarMetadata(connection);

    {
        FakeCalendarEvents failingEvents;
        failingEvents.event = uncachedEvent();
        failingEvents.nextAuthoritativeError = javelin::jmap::OperationError{
            .code = javelin::jmap::OperationErrorCode::NetworkUnavailable,
            .message = QStringLiteral("offline"),
        };
        javelin::app::CalendarNotificationService service{connection, failingEvents};
        service.calendarAlertReceived(QStringLiteral("owner"), QStringLiteral("calendar-account"),
                                      QStringLiteral("event-1"), QStringLiteral("uid-1"), QString{},
                                      QStringLiteral("alert-1"));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        javelin::jmap::cache::CalendarNotificationRepository repository{connection};
        const auto pending = repository.pendingPushed();
        REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::CalendarPushedAlert>>(
            pending));
        CHECK(std::get<std::vector<javelin::jmap::cache::CalendarPushedAlert>>(pending).size() ==
              1);
    }

    FakeCalendarEvents recoveredEvents;
    recoveredEvents.event = uncachedEvent();
    javelin::app::CalendarNotificationService restarted{connection, recoveredEvents};
    QEventLoop loop;
    QString deliveredKey;
    QObject::connect(&restarted, &javelin::app::CalendarNotificationService::reminderDue, &loop,
                     [&deliveredKey, &loop](const QString& key, const QString&, const QString&)
                     {
                         deliveredKey = key;
                         loop.quit();
                     });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(2000);
    restarted.start();
    loop.exec();

    REQUIRE_FALSE(deliveredKey.isEmpty());
    CHECK(recoveredEvents.authoritativeFetches == 1);
    restarted.deliveryAccepted(deliveredKey);
    javelin::jmap::cache::CalendarNotificationRepository repository{connection};
    const auto pending = repository.pendingPushed();
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::cache::CalendarPushedAlert>>(pending));
    CHECK(std::get<std::vector<javelin::jmap::cache::CalendarPushedAlert>>(pending).empty());
}

TEST_CASE("snoozed pushed calendar alerts remain durable until their retry deadline",
          "[app][calendar][notification][push][snooze]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-notification-push-snooze"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    seedCalendarMetadata(connection);

    FakeCalendarEvents calendarEvents;
    calendarEvents.event = uncachedEvent();
    javelin::app::CalendarNotificationService service{connection, calendarEvents};
    QEventLoop loop;
    QString deliveredKey;
    QObject::connect(&service, &javelin::app::CalendarNotificationService::reminderDue, &loop,
                     [&deliveredKey, &loop](const QString& key, const QString&, const QString&)
                     {
                         deliveredKey = key;
                         loop.quit();
                     });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(2000);
    service.calendarAlertReceived(QStringLiteral("owner"), QStringLiteral("calendar-account"),
                                  QStringLiteral("event-1"), QStringLiteral("uid-1"), QString{},
                                  QStringLiteral("alert-1"));
    loop.exec();
    REQUIRE_FALSE(deliveredKey.isEmpty());
    service.deliveryAccepted(deliveredKey);
    service.snooze(deliveredKey);

    javelin::jmap::cache::CalendarNotificationRepository repository{connection};
    const auto pending = repository.pendingPushed();
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::cache::CalendarPushedAlert>>(pending));
    REQUIRE(std::get<std::vector<javelin::jmap::cache::CalendarPushedAlert>>(pending).size() == 1);
    const auto claim =
        repository.claimPushed(deliveredKey.toStdString(), QDateTime::currentDateTimeUtc());
    REQUIRE(std::holds_alternative<javelin::jmap::cache::CalendarPushedAlertClaim>(claim));
    const auto status = std::get<javelin::jmap::cache::CalendarPushedAlertClaim>(claim);
    CHECK_FALSE(status.claimed);
    REQUIRE(status.retryAt.has_value());
    CHECK(*status.retryAt > QDateTime::currentDateTimeUtc());
}

TEST_CASE("calendar alert push waits for calendar metadata before resolving default alerts",
          "[app][calendar][notification][push][metadata]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-notification-push-metadata"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));

    seedOwnerAccount(connection);
    FakeCalendarEvents calendarEvents;
    calendarEvents.event = uncachedEvent();
    calendarEvents.event.useDefaultAlerts = true;
    calendarEvents.event.alerts.clear();
    javelin::app::CalendarNotificationService service{connection, calendarEvents};

    int metadataRequests = 0;
    int deliveries = 0;
    QEventLoop metadataLoop;
    QObject::connect(&service, &javelin::app::CalendarNotificationService::calendarMetadataRequired,
                     &metadataLoop,
                     [&metadataRequests, &metadataLoop](const QString& ownerAccountId)
                     {
                         CHECK(ownerAccountId == QStringLiteral("owner"));
                         ++metadataRequests;
                         metadataLoop.quit();
                     });
    QObject::connect(&service, &javelin::app::CalendarNotificationService::reminderDue,
                     [&deliveries](const QString&, const QString&, const QString&)
                     { ++deliveries; });
    QTimer metadataTimeout;
    metadataTimeout.setSingleShot(true);
    QObject::connect(&metadataTimeout, &QTimer::timeout, &metadataLoop, &QEventLoop::quit);
    metadataTimeout.start(2000);

    service.calendarAlertReceived(QStringLiteral("owner"), QStringLiteral("calendar-account"),
                                  QStringLiteral("event-1"), QStringLiteral("uid-1"), QString{},
                                  QStringLiteral("alert-1"));
    metadataLoop.exec();

    REQUIRE(metadataRequests == 1);
    CHECK(deliveries == 0);
    CHECK(calendarEvents.authoritativeFetches == 1);
    const auto timers = service.findChildren<QTimer*>();
    CHECK(std::ranges::any_of(timers, [](const QTimer* timer)
                              { return timer->isSingleShot() && timer->isActive(); }));

    seedCalendarMetadata(connection, true);
    QEventLoop deliveryLoop;
    QObject::connect(&service, &javelin::app::CalendarNotificationService::reminderDue,
                     &deliveryLoop, [&deliveryLoop](const QString&, const QString&, const QString&)
                     { deliveryLoop.quit(); });
    QTimer deliveryTimeout;
    deliveryTimeout.setSingleShot(true);
    QObject::connect(&deliveryTimeout, &QTimer::timeout, &deliveryLoop, &QEventLoop::quit);
    deliveryTimeout.start(2000);
    service.calendarMetadataReady(QStringLiteral("owner"));
    deliveryLoop.exec();

    CHECK(deliveries == 1);
    CHECK(calendarEvents.authoritativeFetches == 2);
}

TEST_CASE("calendar alert push retries a transient authoritative fetch",
          "[app][calendar][notification][push][retry]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-notification-push-fetch-retry"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    seedCalendarMetadata(connection);

    FakeCalendarEvents calendarEvents;
    calendarEvents.event = uncachedEvent();
    calendarEvents.nextAuthoritativeError = javelin::jmap::OperationError{
        .code = javelin::jmap::OperationErrorCode::NetworkUnavailable,
        .message = QStringLiteral("Temporary network failure"),
    };
    javelin::app::CalendarNotificationService service{connection, calendarEvents};

    int deliveries = 0;
    QObject::connect(&service, &javelin::app::CalendarNotificationService::reminderDue,
                     [&deliveries](const QString&, const QString&, const QString&)
                     { ++deliveries; });
    service.calendarAlertReceived(QStringLiteral("owner"), QStringLiteral("calendar-account"),
                                  QStringLiteral("event-1"), QStringLiteral("uid-1"), QString{},
                                  QStringLiteral("alert-1"));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    REQUIRE(calendarEvents.authoritativeFetches == 1);
    CHECK(deliveries == 0);

    REQUIRE(QMetaObject::invokeMethod(&service, "retryPushedAlerts", Qt::DirectConnection));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    CHECK(calendarEvents.authoritativeFetches == 2);
    CHECK(deliveries == 1);
}

TEST_CASE("calendar alert push retries desktop publication without occurrence materialization",
          "[app][calendar][notification][push][delivery][retry]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-notification-push-delivery-retry"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    seedCalendarMetadata(connection);

    FakeCalendarEvents calendarEvents;
    calendarEvents.event = uncachedEvent();
    javelin::app::CalendarNotificationService service{connection, calendarEvents};

    int deliveries = 0;
    QString deliveredKey;
    QObject::connect(
        &service, &javelin::app::CalendarNotificationService::reminderDue,
        [&deliveries, &deliveredKey](const QString& key, const QString&, const QString&)
        {
            ++deliveries;
            deliveredKey = key;
        });
    service.calendarAlertReceived(QStringLiteral("owner"), QStringLiteral("calendar-account"),
                                  QStringLiteral("event-1"), QStringLiteral("uid-1"), QString{},
                                  QStringLiteral("alert-1"));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    REQUIRE(deliveries == 1);
    REQUIRE_FALSE(deliveredKey.isEmpty());

    service.deliveryFailed(deliveredKey);
    REQUIRE(QMetaObject::invokeMethod(&service, "retryPushedDeliveries", Qt::DirectConnection));
    CHECK(deliveries == 2);
    service.deliveryAccepted(deliveredKey);

    QSqlQuery cachedOccurrences{connection.database()};
    REQUIRE(cachedOccurrences.exec(QStringLiteral("SELECT COUNT(*) FROM calendar_occurrences")));
    REQUIRE(cachedOccurrences.next());
    CHECK(cachedOccurrences.value(0).toInt() == 0);
}
