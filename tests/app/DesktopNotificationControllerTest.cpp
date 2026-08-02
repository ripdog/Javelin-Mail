#include "app/DesktopNotificationController.h"

#include "app/CacheLocationProvider.h"
#include "app/DaemonBackgroundController.h"
#include "app/DaemonServices.h"

#include <QCoreApplication>
#include <QDir>
#include <QMetaObject>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <utility>
#include <variant>

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
            request = NotificationRequest{
                .icon = icon,
                .summary = summary,
                .message = message,
                .actions = actions,
                .hints = hints,
                .timeoutMs = timeoutMs,
            };
            return notificationId;
        }

        void close(const uint closedNotificationId) override
        {
            closedId = closedNotificationId;
        }

        uint notificationId = 73;
        std::optional<NotificationRequest> request;
        std::optional<uint> closedId;
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
