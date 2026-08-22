#include "desktop/notifications/DesktopNotificationController.h"

#include "app/CacheLocationProvider.h"
#include "app/DeferredSendService.h"
#include "daemon/DaemonBackgroundController.h"
#include "daemon/DaemonServices.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QMetaObject>
#include <QSettings>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace
{
    class ApplicationGuard
    {
      public:
        ApplicationGuard()
        {
            if (QCoreApplication::instance() != nullptr)
                return;
            static int argc = 1;
            static char appName[] = "javelin-notification-tests";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    class ScopedSetting
    {
      public:
        ScopedSetting(QString key, const QVariant& value)
            : m_key(std::move(key)), m_previous(m_settings.value(m_key))
        {
            m_settings.setValue(m_key, value);
            m_settings.sync();
        }

        ~ScopedSetting()
        {
            if (m_previous.isValid())
                m_settings.setValue(m_key, m_previous);
            else
                m_settings.remove(m_key);
            m_settings.sync();
        }

      private:
        QSettings m_settings;
        QString m_key;
        QVariant m_previous;
    };

    struct NotificationRequest
    {
        QString icon;
        QString summary;
        QString message;
        QStringList actions;
        QVariantMap hints;
        int timeoutMs = 0;
    };

    class FakeNotificationTransport final : public javelin::app::DesktopNotificationTransport
    {
      public:
        std::variant<uint, QString> send(const QString& icon, const QString& summary,
                                         const QString& message, const QStringList& actions,
                                         const QVariantMap& hints, const int timeoutMs) override
        {
            ++sendCount;
            request = NotificationRequest{
                .icon = icon,
                .summary = summary,
                .message = message,
                .actions = actions,
                .hints = hints,
                .timeoutMs = timeoutMs,
            };
            notificationId = nextNotificationId++;
            if (sendError.has_value())
                return *sendError;
            return notificationId;
        }

        [[nodiscard]] bool supportsActions() const override
        {
            return actionsSupported;
        }

        void close(const uint closedNotificationId) override
        {
            closedId = closedNotificationId;
            closedIds.push_back(closedNotificationId);
            if (closeObserver)
                closeObserver(closedNotificationId);
        }

        uint nextNotificationId = 73;
        uint notificationId = 73;
        std::size_t sendCount = 0;
        bool actionsSupported = true;
        std::optional<QString> sendError;
        std::optional<NotificationRequest> request;
        std::optional<uint> closedId;
        std::vector<uint> closedIds;
        std::function<void(uint)> closeObserver;
    };
} // namespace

TEST_CASE("mail notification activation preserves the message route and mailbox name",
          "[app][daemon][notification][activation]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    const auto cacheRoot = temporaryDirectory.filePath(QStringLiteral("cache"));
    REQUIRE(QDir{}.mkpath(cacheRoot));

    auto location = javelin::app::CacheLocationProvider{cacheRoot}.loadOrCreate();
    REQUIRE(std::holds_alternative<javelin::app::CacheLocation>(location));
    javelin::app::DaemonServices services{
        std::get<javelin::app::CacheLocation>(std::move(location))};

    auto transport = std::make_unique<FakeNotificationTransport>();
    auto* transportObserver = transport.get();
    auto notifications =
        std::make_unique<javelin::app::DesktopNotificationController>(std::move(transport), false);
    auto* notificationController = notifications.get();
    javelin::app::DaemonBackgroundController background{services, std::move(notifications)};

    std::optional<javelin::protocol::ActivationRoute> activatedRoute;
    QObject::connect(&background, &javelin::app::DaemonBackgroundController::activationRequested,
                     &background, [&activatedRoute](javelin::protocol::ActivationRoute route)
                     { activatedRoute = std::move(route); });

    REQUIRE(notificationController->notifyNewMail(
        QStringLiteral("account-1"), QStringLiteral("projects"), QStringLiteral("thread-8"),
        QStringLiteral("email-13"), QStringLiteral("Projects"),
        QStringLiteral("New mail in Projects"), QStringLiteral("Test subject")));
    REQUIRE(transportObserver->request.has_value());
    CHECK(transportObserver->request->icon == QStringLiteral("mail-unread"));
    CHECK(transportObserver->request->summary == QStringLiteral("New mail in Projects"));
    CHECK(transportObserver->request->message == QStringLiteral("Test subject"));
    CHECK(transportObserver->request->actions ==
          QStringList{QStringLiteral("default"), QStringLiteral("Open")});
    CHECK(transportObserver->request->hints.value(QStringLiteral("desktop-entry")).toString() ==
          QStringLiteral("javelinmail"));

    REQUIRE(QMetaObject::invokeMethod(notificationController, "onActivationToken",
                                      Qt::DirectConnection,
                                      Q_ARG(uint, transportObserver->notificationId),
                                      Q_ARG(QString, QStringLiteral("token-21"))));
    REQUIRE(QMetaObject::invokeMethod(
        notificationController, "onActionInvoked", Qt::DirectConnection,
        Q_ARG(uint, transportObserver->notificationId), Q_ARG(QString, QStringLiteral("default"))));

    REQUIRE(activatedRoute.has_value());
    const auto* messageRoute = std::get_if<javelin::protocol::OpenMessageRoute>(&*activatedRoute);
    REQUIRE(messageRoute != nullptr);
    CHECK(messageRoute->accountId == QStringLiteral("account-1"));
    CHECK(messageRoute->mailboxId == QStringLiteral("projects"));
    CHECK(messageRoute->mailboxName == QStringLiteral("Projects"));
    CHECK(messageRoute->threadId == QStringLiteral("thread-8"));
    CHECK(messageRoute->emailId == QStringLiteral("email-13"));
    CHECK(messageRoute->activationToken == QStringLiteral("token-21"));
}

TEST_CASE("calendar invitation notification is persistent open-only and activates its event",
          "[app][daemon][notification][calendar][invitation]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto transport = std::make_unique<FakeNotificationTransport>();
    auto* observer = transport.get();
    javelin::app::DesktopNotificationController controller{std::move(transport), false};

    QString activatedKey;
    QString activatedAccount;
    QString activatedEvent;
    QString activatedRecurrence;
    QString activatedDate;
    QString activatedToken;
    QObject::connect(&controller,
                     &javelin::app::DesktopNotificationController::calendarInvitationActivated,
                     &controller,
                     [&](const QString& key, const QString& accountId, const QString& eventId,
                         const QString& recurrenceId, const QString& navigationDate,
                         const QString& activationToken)
                     {
                         activatedKey = key;
                         activatedAccount = accountId;
                         activatedEvent = eventId;
                         activatedRecurrence = recurrenceId;
                         activatedDate = navigationDate;
                         activatedToken = activationToken;
                     });

    REQUIRE(controller.notifyCalendarInvitation(
        QStringLiteral("invitation-key"), QStringLiteral("calendar-account"),
        QStringLiteral("event-42"), QStringLiteral("2026-08-21T09:30:00"),
        QStringLiteral("2026-08-21"), QStringLiteral("Planning call"),
        QStringLiteral("From Organizer\n21/08/2026, 9:30 am")));
    REQUIRE(observer->request.has_value());
    CHECK(observer->request->icon == QStringLiteral("x-office-calendar"));
    CHECK(observer->request->summary == QStringLiteral("Calendar invitation: Planning call"));
    CHECK(observer->request->actions ==
          QStringList{QStringLiteral("default"), QStringLiteral("Open")});
    CHECK(observer->request->timeoutMs == 0);
    CHECK_FALSE(observer->request->hints.value(QStringLiteral("transient")).toBool());

    REQUIRE(QMetaObject::invokeMethod(&controller, "onActivationToken", Qt::DirectConnection,
                                      Q_ARG(uint, observer->notificationId),
                                      Q_ARG(QString, QStringLiteral("activation-token"))));
    REQUIRE(QMetaObject::invokeMethod(&controller, "onActionInvoked", Qt::DirectConnection,
                                      Q_ARG(uint, observer->notificationId),
                                      Q_ARG(QString, QStringLiteral("default"))));
    CHECK(activatedKey == QStringLiteral("invitation-key"));
    CHECK(activatedAccount == QStringLiteral("calendar-account"));
    CHECK(activatedEvent == QStringLiteral("event-42"));
    CHECK(activatedRecurrence == QStringLiteral("2026-08-21T09:30:00"));
    CHECK(activatedDate == QStringLiteral("2026-08-21"));
    CHECK(activatedToken == QStringLiteral("activation-token"));

    controller.closeCalendarInvitation(QStringLiteral("invitation-key"));
    CHECK(observer->closedId == observer->notificationId);
}

TEST_CASE("undoable send notification reports its actionable lifetime and timeout",
          "[app][daemon][notification][deferred-send]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto transport = std::make_unique<FakeNotificationTransport>();
    auto* observer = transport.get();
    javelin::app::DesktopNotificationController controller{std::move(transport), false, true};

    CHECK(controller.notifyUndoableSend(QStringLiteral("send-1"), QStringLiteral("Scheduled"),
                                        QStringLiteral("Subject"), 12'345));
    REQUIRE(observer->request.has_value());
    CHECK(observer->request->timeoutMs == 12'345);
    CHECK(observer->request->actions ==
          QStringList{QStringLiteral("undo-send:send-1"), QStringLiteral("Undo Send")});
    CHECK(observer->request->hints.value(QStringLiteral("transient")).toBool());
}

TEST_CASE("dialog undo send mode routes a deadline without creating a notification",
          "[app][daemon][notification][deferred-send]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    const auto cacheRoot = temporaryDirectory.filePath(QStringLiteral("cache"));
    REQUIRE(QDir{}.mkpath(cacheRoot));

    auto location = javelin::app::CacheLocationProvider{cacheRoot}.loadOrCreate();
    REQUIRE(std::holds_alternative<javelin::app::CacheLocation>(location));
    javelin::app::DaemonServices services{
        std::get<javelin::app::CacheLocation>(std::move(location))};
    const ScopedSetting presentation{QStringLiteral("compose/undoSendUsesDialog"), true};
    REQUIRE(QSettings{}.value(QStringLiteral("compose/undoSendUsesDialog")).toBool());

    auto transport = std::make_unique<FakeNotificationTransport>();
    auto* transportObserver = transport.get();
    auto notifications =
        std::make_unique<javelin::app::DesktopNotificationController>(std::move(transport), false);
    javelin::app::DaemonBackgroundController background{services, std::move(notifications)};
    background.start(false);

    std::optional<javelin::protocol::ActivationRoute> activatedRoute;
    QObject::connect(&background, &javelin::app::DaemonBackgroundController::activationRequested,
                     &background, [&activatedRoute](javelin::protocol::ActivationRoute route)
                     { activatedRoute = std::move(route); });

    const auto before = QDateTime::currentMSecsSinceEpoch();
    Q_EMIT services.deferredSendService().undoableSendScheduled(
        QStringLiteral("send-dialog"), QStringLiteral("Message scheduled"),
        QStringLiteral("Send “Quarterly report”"), 5'000);

    CHECK_FALSE(transportObserver->request.has_value());
    REQUIRE(activatedRoute.has_value());
    const auto* dialogRoute =
        std::get_if<javelin::protocol::ShowUndoSendDialogRoute>(&*activatedRoute);
    REQUIRE(dialogRoute != nullptr);
    CHECK(dialogRoute->sendId == QStringLiteral("send-dialog"));
    CHECK(dialogRoute->title == QStringLiteral("Message scheduled"));
    CHECK(dialogRoute->message == QStringLiteral("Send “Quarterly report”"));
    CHECK(dialogRoute->deadlineEpochMilliseconds >= before + 5'000);
    CHECK(dialogRoute->deadlineEpochMilliseconds <= QDateTime::currentMSecsSinceEpoch() + 5'000);
}

TEST_CASE("undoable send notification does not gate when delivery or actions are unavailable",
          "[app][daemon][notification][deferred-send]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    SECTION("transport failure")
    {
        auto transport = std::make_unique<FakeNotificationTransport>();
        transport->sendError = QStringLiteral("notification failed");
        javelin::app::DesktopNotificationController controller{std::move(transport), false, true};
        CHECK_FALSE(controller.notifyUndoableSend(QStringLiteral("send-1"),
                                                  QStringLiteral("Scheduled"),
                                                  QStringLiteral("Subject"), 1'000));
    }

    SECTION("missing action capability")
    {
        auto transport = std::make_unique<FakeNotificationTransport>();
        auto* observer = transport.get();
        transport->actionsSupported = false;
        javelin::app::DesktopNotificationController controller{std::move(transport), false, true};
        CHECK_FALSE(controller.notifyUndoableSend(QStringLiteral("send-1"),
                                                  QStringLiteral("Scheduled"),
                                                  QStringLiteral("Subject"), 1'000));
        CHECK(observer->sendCount == 0);
        CHECK_FALSE(observer->request.has_value());
    }

    SECTION("failed signal subscription")
    {
        auto transport = std::make_unique<FakeNotificationTransport>();
        auto* observer = transport.get();
        javelin::app::DesktopNotificationController controller{std::move(transport), false, false};
        CHECK_FALSE(controller.notifyUndoableSend(QStringLiteral("send-1"),
                                                  QStringLiteral("Scheduled"),
                                                  QStringLiteral("Subject"), 1'000));
        CHECK(observer->sendCount == 0);
        CHECK_FALSE(observer->request.has_value());
    }
}

TEST_CASE("losing notification action support removes an existing Undo popup without replacement",
          "[app][daemon][notification][deferred-send]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto transport = std::make_unique<FakeNotificationTransport>();
    auto* observer = transport.get();
    javelin::app::DesktopNotificationController controller{std::move(transport), false, true};

    REQUIRE(controller.notifyUndoableSend(QStringLiteral("send-1"), QStringLiteral("Scheduled"),
                                          QStringLiteral("Subject"), 1'000));
    const auto originalNotificationId = observer->notificationId;
    CHECK(observer->sendCount == 1);

    observer->actionsSupported = false;
    observer->request.reset();
    CHECK_FALSE(controller.notifyUndoableSend(QStringLiteral("send-1"), QStringLiteral("Scheduled"),
                                              QStringLiteral("Subject"), 1'000));
    CHECK(observer->closedId == originalNotificationId);
    CHECK(observer->sendCount == 1);
    CHECK_FALSE(observer->request.has_value());
}

TEST_CASE("undoable send closure reasons end the window exactly once",
          "[app][daemon][notification][deferred-send]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto transport = std::make_unique<FakeNotificationTransport>();
    auto* observer = transport.get();
    javelin::app::DesktopNotificationController controller{std::move(transport), false, true};
    std::vector<std::pair<QString, javelin::app::DesktopNotificationCloseReason>> ended;
    QObject::connect(
        &controller, &javelin::app::DesktopNotificationController::undoableSendWindowEnded,
        &controller,
        [&ended](const QString& sendId, const javelin::app::DesktopNotificationCloseReason reason)
        { ended.emplace_back(sendId, reason); });

    REQUIRE(controller.notifyUndoableSend(QStringLiteral("expired"), QStringLiteral("Scheduled"),
                                          QStringLiteral("Subject"), 1'000));
    REQUIRE(QMetaObject::invokeMethod(&controller, "onNotificationClosed", Qt::DirectConnection,
                                      Q_ARG(uint, observer->notificationId), Q_ARG(uint, 1U)));
    REQUIRE(controller.notifyUndoableSend(QStringLiteral("dismissed"), QStringLiteral("Scheduled"),
                                          QStringLiteral("Subject"), 1'000));
    REQUIRE(QMetaObject::invokeMethod(&controller, "onNotificationClosed", Qt::DirectConnection,
                                      Q_ARG(uint, observer->notificationId), Q_ARG(uint, 2U)));
    REQUIRE(controller.notifyUndoableSend(QStringLiteral("undefined"), QStringLiteral("Scheduled"),
                                          QStringLiteral("Subject"), 1'000));
    REQUIRE(QMetaObject::invokeMethod(&controller, "onNotificationClosed", Qt::DirectConnection,
                                      Q_ARG(uint, observer->notificationId), Q_ARG(uint, 4U)));
    REQUIRE(controller.notifyUndoableSend(QStringLiteral("application"),
                                          QStringLiteral("Scheduled"), QStringLiteral("Subject"),
                                          1'000));
    REQUIRE(QMetaObject::invokeMethod(&controller, "onNotificationClosed", Qt::DirectConnection,
                                      Q_ARG(uint, observer->notificationId), Q_ARG(uint, 3U)));

    REQUIRE(ended.size() == 3);
    CHECK(ended[0] == std::pair{QStringLiteral("expired"),
                                javelin::app::DesktopNotificationCloseReason::Expired});
    CHECK(ended[1] == std::pair{QStringLiteral("dismissed"),
                                javelin::app::DesktopNotificationCloseReason::DismissedByUser});
    CHECK(ended[2] == std::pair{QStringLiteral("undefined"),
                                javelin::app::DesktopNotificationCloseReason::Undefined});
}

TEST_CASE("programmatic and duplicate notification closure cannot release a send",
          "[app][daemon][notification][deferred-send]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto transport = std::make_unique<FakeNotificationTransport>();
    auto* observer = transport.get();
    javelin::app::DesktopNotificationController controller{std::move(transport), false, true};
    int ended = 0;
    QObject::connect(&controller,
                     &javelin::app::DesktopNotificationController::undoableSendWindowEnded,
                     &controller, [&ended](const QString&, auto) { ++ended; });

    REQUIRE(controller.notifyUndoableSend(QStringLiteral("send-1"), QStringLiteral("Scheduled"),
                                          QStringLiteral("Subject"), 1'000));
    controller.closeUndoableSendNotification(QStringLiteral("send-1"));
    CHECK(observer->closedId == observer->notificationId);
    CHECK(ended == 0);
    REQUIRE(QMetaObject::invokeMethod(&controller, "onNotificationClosed", Qt::DirectConnection,
                                      Q_ARG(uint, observer->notificationId), Q_ARG(uint, 1U)));
    CHECK(ended == 0);

    REQUIRE(controller.notifyUndoableSend(QStringLiteral("send-2"), QStringLiteral("Scheduled"),
                                          QStringLiteral("Subject"), 1'000));
    REQUIRE(QMetaObject::invokeMethod(&controller, "onNotificationClosed", Qt::DirectConnection,
                                      Q_ARG(uint, observer->notificationId), Q_ARG(uint, 1U)));
    REQUIRE(QMetaObject::invokeMethod(&controller, "onNotificationClosed", Qt::DirectConnection,
                                      Q_ARG(uint, observer->notificationId), Q_ARG(uint, 1U)));
    CHECK(ended == 1);
}

TEST_CASE("undo action consumes tracking before requesting cancellation",
          "[app][daemon][notification][deferred-send]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto transport = std::make_unique<FakeNotificationTransport>();
    auto* observer = transport.get();
    javelin::app::DesktopNotificationController controller{std::move(transport), false, true};
    int undoRequests = 0;
    int ended = 0;
    QObject::connect(&controller, &javelin::app::DesktopNotificationController::undoSendRequested,
                     &controller,
                     [&controller, &undoRequests](const QString&)
                     {
                         ++undoRequests;
                         static_cast<void>(QMetaObject::invokeMethod(
                             &controller, "onNotificationClosed", Qt::DirectConnection,
                             Q_ARG(uint, 73U), Q_ARG(uint, 1U)));
                     });
    QObject::connect(&controller,
                     &javelin::app::DesktopNotificationController::undoableSendWindowEnded,
                     &controller, [&ended](const QString&, auto) { ++ended; });

    REQUIRE(controller.notifyUndoableSend(QStringLiteral("send-1"), QStringLiteral("Scheduled"),
                                          QStringLiteral("Subject"), 1'000));
    REQUIRE(QMetaObject::invokeMethod(&controller, "onActionInvoked", Qt::DirectConnection,
                                      Q_ARG(uint, observer->notificationId),
                                      Q_ARG(QString, QStringLiteral("undo-send:send-1"))));
    CHECK(undoRequests == 1);
    CHECK(ended == 0);
    REQUIRE(QMetaObject::invokeMethod(&controller, "onNotificationClosed", Qt::DirectConnection,
                                      Q_ARG(uint, observer->notificationId), Q_ARG(uint, 1U)));
    CHECK(undoRequests == 1);
}

TEST_CASE("self-describing Undo action survives an untracked notification",
          "[app][daemon][notification][deferred-send]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto transport = std::make_unique<FakeNotificationTransport>();
    javelin::app::DesktopNotificationController controller{std::move(transport), false, true};
    std::vector<QString> requested;
    QObject::connect(&controller, &javelin::app::DesktopNotificationController::undoSendRequested,
                     &controller,
                     [&requested](const QString& sendId) { requested.push_back(sendId); });

    REQUIRE(QMetaObject::invokeMethod(
        &controller, "onActionInvoked", Qt::DirectConnection, Q_ARG(uint, 901U),
        Q_ARG(QString, QStringLiteral("undo-send:previous-process-send"))));
    REQUIRE(QMetaObject::invokeMethod(&controller, "onActionInvoked", Qt::DirectConnection,
                                      Q_ARG(uint, 902U),
                                      Q_ARG(QString, QStringLiteral("undo-send:"))));
    REQUIRE(QMetaObject::invokeMethod(
        &controller, "onActionInvoked", Qt::DirectConnection, Q_ARG(uint, 903U),
        Q_ARG(QString, QStringLiteral("undo-send:send:with-extra-part"))));
    REQUIRE(requested.size() == 1);
    CHECK(requested.front() == QStringLiteral("previous-process-send"));
}

TEST_CASE("notification service loss ends every active Undo window once",
          "[app][daemon][notification][deferred-send]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto transport = std::make_unique<FakeNotificationTransport>();
    auto* observer = transport.get();
    javelin::app::DesktopNotificationController controller{std::move(transport), false, true};
    std::vector<QString> ended;
    QObject::connect(
        &controller, &javelin::app::DesktopNotificationController::undoableSendWindowEnded,
        &controller,
        [&ended](const QString& sendId, const javelin::app::DesktopNotificationCloseReason reason)
        {
            CHECK(reason == javelin::app::DesktopNotificationCloseReason::NotificationServiceLost);
            ended.push_back(sendId);
        });

    REQUIRE(controller.notifyUndoableSend(QStringLiteral("send-1"), QStringLiteral("Scheduled"),
                                          QStringLiteral("Subject"), 1'000));
    REQUIRE(controller.notifyUndoableSend(QStringLiteral("send-2"), QStringLiteral("Scheduled"),
                                          QStringLiteral("Subject"), 1'000));
    REQUIRE(QMetaObject::invokeMethod(
        &controller, "onNotificationServiceUnregistered", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("org.freedesktop.Notifications"))));
    CHECK(ended.size() == 2);
    CHECK(observer->closedIds.empty());
    REQUIRE(QMetaObject::invokeMethod(
        &controller, "onNotificationServiceUnregistered", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("org.freedesktop.Notifications"))));
    CHECK(ended.size() == 2);
}

TEST_CASE("notification replacement untracks before closing the old notification",
          "[app][daemon][notification][deferred-send]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto transport = std::make_unique<FakeNotificationTransport>();
    auto* observer = transport.get();
    javelin::app::DesktopNotificationController* controller = nullptr;
    observer->closeObserver = [&controller](const uint notificationId)
    {
        static_cast<void>(QMetaObject::invokeMethod(controller, "onNotificationClosed",
                                                    Qt::DirectConnection,
                                                    Q_ARG(uint, notificationId), Q_ARG(uint, 1U)));
    };
    javelin::app::DesktopNotificationController instance{std::move(transport), false, true};
    controller = &instance;
    int ended = 0;
    QObject::connect(&instance,
                     &javelin::app::DesktopNotificationController::undoableSendWindowEnded,
                     &instance, [&ended](const QString&, auto) { ++ended; });

    REQUIRE(instance.notifyUndoableSend(QStringLiteral("send-1"), QStringLiteral("Scheduled"),
                                        QStringLiteral("Subject"), 1'000));
    REQUIRE(instance.notifyUndoableSend(QStringLiteral("send-1"), QStringLiteral("Scheduled"),
                                        QStringLiteral("Subject"), 1'000));
    CHECK(ended == 0);
}
