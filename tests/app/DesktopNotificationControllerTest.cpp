#include "desktop/notifications/DesktopNotificationController.h"
#include "desktop/notifications/KdeNotificationIntegration.h"

#include "app/AccountRuntimeManager.h"
#include "app/CacheLocationProvider.h"
#include "app/DeferredSendService.h"
#include "app/FullMailSyncService.h"
#include "app/MailMutationApplicationService.h"
#include "app/MailNotificationService.h"
#include "app/MessageContentApplicationService.h"
#include "daemon/DaemonBackgroundController.h"
#include "daemon/DaemonServices.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/NotificationRepository.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QMetaObject>
#include <QSettings>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
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

    class ScopedEnvironmentVariable
    {
      public:
        ScopedEnvironmentVariable(QByteArray name, QByteArray value)
            : m_name(std::move(name)), m_wasSet(qEnvironmentVariableIsSet(m_name.constData())),
              m_previous(qgetenv(m_name.constData()))
        {
            qputenv(m_name.constData(), value);
        }

        ~ScopedEnvironmentVariable()
        {
            if (m_wasSet)
                qputenv(m_name.constData(), m_previous);
            else
                qunsetenv(m_name.constData());
        }

      private:
        QByteArray m_name;
        bool m_wasSet = false;
        QByteArray m_previous;
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

    class ScopedKdeNotificationConfig
    {
      public:
        ScopedKdeNotificationConfig(const QString& applicationName, const QByteArray& installed,
                                    const QByteArray& user)
            : m_applicationName(applicationName)
        {
            QStandardPaths::setTestModeEnabled(true);
            const auto dataDirectory =
                QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                QStringLiteral("/knotifications6");
            const auto configDirectory =
                QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
            REQUIRE(QDir{}.mkpath(dataDirectory));
            REQUIRE(QDir{}.mkpath(configDirectory));
            m_installedPath =
                dataDirectory + QLatin1Char('/') + m_applicationName + QStringLiteral(".notifyrc");
            m_userPath = configDirectory + QLatin1Char('/') + m_applicationName +
                         QStringLiteral(".notifyrc");
            writeFile(m_installedPath, installed);
            writeFile(m_userPath, user);
        }

        ~ScopedKdeNotificationConfig()
        {
            QFile::remove(m_installedPath);
            QFile::remove(m_userPath);
            QStandardPaths::setTestModeEnabled(false);
        }

      private:
        static void writeFile(const QString& path, const QByteArray& contents)
        {
            QFile file{path};
            REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            REQUIRE(file.write(contents) == contents.size());
            file.close();
        }

        QString m_applicationName;
        QString m_installedPath;
        QString m_userPath;
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
        javelin::app::DesktopNotificationDeliveryResult
        send(const QString& icon, const QString& summary, const QString& message,
             const QStringList& actions, const QVariantMap& hints, const int timeoutMs) override
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
            if (deliverWithoutPopup)
                return javelin::app::DesktopNotificationDelivery{.notificationId = std::nullopt};
            return javelin::app::DesktopNotificationDelivery{.notificationId = notificationId};
        }

        [[nodiscard]] bool supportsActions() const override
        {
            ++supportQueryCount;
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
        mutable std::size_t supportQueryCount = 0;
        bool actionsSupported = true;
        bool deliverWithoutPopup = false;
        std::optional<QString> sendError;
        std::optional<NotificationRequest> request;
        std::optional<uint> closedId;
        std::vector<uint> closedIds;
        std::function<void(uint)> closeObserver;
    };

    void seedPendingMailNotification(javelin::app::DaemonServices& services)
    {
        auto& connection = services.databaseConnection();
        QSqlQuery account{connection.database()};
        REQUIRE(account.exec(QStringLiteral(
            "INSERT INTO accounts(account_id,email_address,session_url,is_primary,cap_mail) "
            "VALUES('account-1','user@example.test','https://example.test/jmap',1,1)")));

        const javelin::jmap::domain::MailboxRights rights{
            .mayReadItems = true,
            .mayAddItems = true,
            .mayRemoveItems = true,
            .maySetSeen = true,
            .maySetKeywords = true,
        };
        javelin::jmap::domain::Mailbox inbox;
        inbox.id = "inbox";
        inbox.name = "Inbox";
        inbox.role = "inbox";
        inbox.totalEmails = 1;
        inbox.unreadEmails = 1;
        inbox.totalThreads = 1;
        inbox.unreadThreads = 1;
        inbox.isSubscribed = true;
        inbox.myRights = rights;
        javelin::jmap::domain::Mailbox archive;
        archive.id = "archive";
        archive.name = "Archive";
        archive.role = "archive";
        archive.isSubscribed = true;
        archive.myRights = rights;
        javelin::jmap::cache::MailboxRepository mailboxes{connection};
        REQUIRE_FALSE(mailboxes.replaceAll("account-1", {inbox, archive}).has_value());

        javelin::jmap::domain::Email email;
        email.id = "email-1";
        email.threadId = "thread-1";
        email.mailboxIds = {"inbox"};
        email.receivedAt = "2026-08-28T00:00:00Z";
        email.subject = "Background notification";
        javelin::jmap::cache::EmailRepository emails{connection};
        REQUIRE_FALSE(emails.upsertMany("account-1", {email}).has_value());

        javelin::jmap::cache::NotificationRepository notifications{connection};
        REQUIRE_FALSE(notifications.replaceActiveMailboxes("account-1", {"inbox"}).has_value());
        auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
            connection, QStringLiteral("Seed background notification"));
        REQUIRE(
            std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(transactionResult));
        auto transaction =
            std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));
        const auto created =
            notifications.createEventIfUnconsumed(transaction, "account-1",
                                                  {.mailboxId = "inbox",
                                                   .emailId = email.id,
                                                   .threadId = email.threadId,
                                                   .subject = email.subject,
                                                   .receivedAt = email.receivedAt});
        REQUIRE(std::holds_alternative<bool>(created));
        REQUIRE(std::get<bool>(created));
        REQUIRE_FALSE(transaction.commit().has_value());
    }

    [[nodiscard]] int rowCount(javelin::app::DaemonServices& services, const QString& table)
    {
        QSqlQuery query{services.databaseConnection().database()};
        REQUIRE(query.exec(QStringLiteral("SELECT COUNT(*) FROM %1").arg(table)));
        REQUIRE(query.next());
        return query.value(0).toInt();
    }
} // namespace

TEST_CASE("daemon background honors explicit offline catch-up scope on settlement publication",
          "[app][daemon][mail-background][offline]")
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
    auto& connection = services.databaseConnection();

    QSqlQuery account{connection.database()};
    REQUIRE(account.exec(QStringLiteral(
        "INSERT INTO accounts(account_id,email_address,session_url,is_primary,cap_mail) "
        "VALUES('account-1','user@example.test','https://example.test/jmap',1,1)")));
    QSqlQuery mailbox{connection.database()};
    REQUIRE(mailbox.exec(QStringLiteral(
        "INSERT INTO mailboxes(account_id,mailbox_id,name,role,total_emails,total_threads,"
        "is_subscribed) VALUES('account-1','archive','Archive','archive',1,1,1),"
        "('account-1','inbox','Inbox','inbox',0,0,1)")));
    javelin::jmap::domain::Email email;
    email.id = "email-1";
    email.blobId = "blob-1";
    email.threadId = "thread-1";
    email.mailboxIds = {"archive"};
    email.receivedAt = "2026-09-06T00:00:00Z";
    email.subject = "Offline catch-up";
    javelin::jmap::cache::EmailRepository emails{connection};
    REQUIRE_FALSE(emails.upsertMany("account-1", {email}).has_value());

    services.fullMailSyncService().applySettings({javelin::app::FullSyncAccountConfiguration{
        .settings = {.connectionId = "connection-1",
                     .revision = 1,
                     .sessionUrl = "https://example.test/.well-known/jmap",
                     .loginEmail = "user@example.test",
                     .apiKey = "token",
                     .refreshToken = {},
                     .tokenEndpoint = {},
                     .oauthClientId = {}},
        .accountId = "account-1",
        .mailboxIds = {"archive", "inbox"},
    }});
    QSqlQuery complete{connection.database()};
    REQUIRE(complete.exec(
        QStringLiteral("UPDATE offline_mailbox_scopes SET status='complete',generation=1,"
                       "completed_generation=1,query_state='query-state',email_state='email-state' "
                       "WHERE account_id='account-1'")));
    QSqlQuery clearMembership{connection.database()};
    REQUIRE(clearMembership.exec(
        QStringLiteral("DELETE FROM offline_mailbox_membership WHERE account_id='account-1'")));
    QSqlQuery staleSourceMembership{connection.database()};
    REQUIRE(staleSourceMembership.exec(QStringLiteral(
        "INSERT INTO "
        "offline_mailbox_membership(account_id,mailbox_id,email_id,generation,position) "
        "VALUES('account-1','inbox','email-1',1,0)")));

    auto notifications = std::make_unique<javelin::app::DesktopNotificationController>(
        std::make_unique<FakeNotificationTransport>(), false, true);
    javelin::app::DaemonBackgroundController background{services, std::move(notifications)};
    background.start(false);

    services.fullMailSyncService().mailCommitEffectsCommitted(
        QStringLiteral("account-1"), javelin::jmap::sync::MailCommitEffects{
                                         .emailObjectsChanged = false,
                                         .mailboxMembershipChanged = true,
                                         .sourceIdentityChanged = false,
                                         .affectedMailboxIds = {"archive", "inbox"},
                                     });

    QSqlQuery membership{connection.database()};
    REQUIRE(membership.exec(QStringLiteral(
        "SELECT mailbox_id,COUNT(*) FROM offline_mailbox_membership WHERE "
        "account_id='account-1' AND email_id='email-1' GROUP BY mailbox_id ORDER BY mailbox_id")));
    REQUIRE(membership.next());
    CHECK(membership.value(0).toString() == QStringLiteral("archive"));
    CHECK(membership.value(1).toInt() == 1);
    CHECK_FALSE(membership.next());
}

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
    auto notifications = std::make_unique<javelin::app::DesktopNotificationController>(
        std::move(transport), false, true);
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
          QStringList{QStringLiteral("default"), QStringLiteral("Open"), QStringLiteral("archive"),
                      QStringLiteral("Archive"), QStringLiteral("mark-read"),
                      QStringLiteral("Mark Read"), QStringLiteral("reply"),
                      QStringLiteral("Reply")});
    CHECK_FALSE(transportObserver->request->hints.contains(QStringLiteral("desktop-entry")));
    CHECK(transportObserver->request->hints.value(QStringLiteral("x-kde-appname")).toString() ==
          QStringLiteral("javelinmail"));
    CHECK(transportObserver->request->hints.value(QStringLiteral("x-kde-eventId")).toString() ==
          QStringLiteral("new-mail"));
    CHECK(transportObserver->request->hints.value(QStringLiteral("category")).toString() ==
          QStringLiteral("email.arrived"));
    CHECK(transportObserver->request->hints.value(QStringLiteral("sound-name")).toString() ==
          QStringLiteral("message-new-instant"));

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

    REQUIRE(QMetaObject::invokeMethod(
        notificationController, "onActionInvoked", Qt::DirectConnection,
        Q_ARG(uint, transportObserver->notificationId), Q_ARG(QString, QStringLiteral("reply"))));
    REQUIRE(activatedRoute.has_value());
    const auto* replyRoute = std::get_if<javelin::protocol::ReplyMessageRoute>(&*activatedRoute);
    REQUIRE(replyRoute != nullptr);
    CHECK(replyRoute->accountId == QStringLiteral("account-1"));
    CHECK(replyRoute->emailId == QStringLiteral("email-13"));
    CHECK(replyRoute->activationToken == QStringLiteral("token-21"));
    CHECK(transportObserver->closedId == transportObserver->notificationId);
}

TEST_CASE("daemon delivers pending new mail while no GUI is running",
          "[app][daemon][notification][background][gui-closed]")
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
    seedPendingMailNotification(services);
    CHECK(services.accountRuntimeManager().configuredAccountIds().empty());

    auto transport = std::make_unique<FakeNotificationTransport>();
    auto* observer = transport.get();
    auto notifications = std::make_unique<javelin::app::DesktopNotificationController>(
        std::move(transport), false, true);
    javelin::app::DaemonBackgroundController background{services, std::move(notifications)};
    background.start(false);

    services.mailNotificationService().accountChanged(QStringLiteral("account-1"));

    REQUIRE(observer->request.has_value());
    CHECK(observer->sendCount == 1);
    CHECK(observer->request->summary == QStringLiteral("New mail in Inbox"));
    CHECK(observer->request->message == QStringLiteral("Background notification"));
    CHECK(rowCount(services, QStringLiteral("mail_notification_event_outbox")) == 0);
    CHECK(rowCount(services, QStringLiteral("notification_dispatch_claims")) == 0);
}

TEST_CASE("raw message availability queues search indexing through daemon background dependencies",
          "[app][daemon][background][mail-index]")
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
    QSqlQuery account{services.databaseConnection().database()};
    REQUIRE(account.exec(QStringLiteral(
        "INSERT INTO accounts(account_id,email_address,session_url,is_primary,cap_mail) "
        "VALUES('account-1','user@example.test','https://example.test/jmap',1,1)")));

    auto transport = std::make_unique<FakeNotificationTransport>();
    auto notifications = std::make_unique<javelin::app::DesktopNotificationController>(
        std::move(transport), false, true);
    javelin::app::DaemonBackgroundController background{services, std::move(notifications)};
    background.start(false);

    services.messageContentApplicationService().publishMessageContentCommitted(
        QStringLiteral("account-1"), QStringLiteral("email-1"));

    QSqlQuery jobs{services.databaseConnection().database()};
    REQUIRE(jobs.exec(
        QStringLiteral("SELECT COUNT(*) FROM background_jobs WHERE account_id='account-1' AND "
                       "kind='search_index'")));
    REQUIRE(jobs.next());
    CHECK(jobs.value(0).toInt() == 1);
}

TEST_CASE("failed daemon desktop delivery retries from local notification state without sync",
          "[app][daemon][notification][background][retry][gui-closed]")
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
    seedPendingMailNotification(services);
    REQUIRE(services.accountRuntimeManager().configuredAccountIds().empty());

    auto transport = std::make_unique<FakeNotificationTransport>();
    auto* observer = transport.get();
    observer->sendError = QStringLiteral("desktop notification unavailable");
    auto notifications = std::make_unique<javelin::app::DesktopNotificationController>(
        std::move(transport), false, true);
    auto* notificationController = notifications.get();
    javelin::app::DaemonBackgroundController background{services, std::move(notifications)};
    background.start(false);

    services.mailNotificationService().accountChanged(QStringLiteral("account-1"));
    CHECK(observer->sendCount == 1);
    CHECK(rowCount(services, QStringLiteral("mail_notification_event_outbox")) == 1);
    CHECK(rowCount(services, QStringLiteral("notification_dispatch_claims")) == 0);

    // Reattaching the same delivery port makes an already queued service-owned retry immediately
    // eligible, without relying on account synchronization or controller-owned retry state.
    observer->sendError.reset();
    services.mailNotificationService().setDeliveryPort(nullptr);
    services.mailNotificationService().setDeliveryPort(notificationController);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    CHECK(observer->sendCount == 2);
    CHECK(rowCount(services, QStringLiteral("mail_notification_event_outbox")) == 0);
    CHECK(rowCount(services, QStringLiteral("notification_dispatch_claims")) == 0);
    CHECK(services.accountRuntimeManager().configuredAccountIds().empty());
}

TEST_CASE("new mail actions mutate through the daemon while no GUI is running",
          "[app][daemon][notification][actions][gui-closed]")
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
    seedPendingMailNotification(services);

    auto transport = std::make_unique<FakeNotificationTransport>();
    auto* observer = transport.get();
    auto notifications = std::make_unique<javelin::app::DesktopNotificationController>(
        std::move(transport), false, true);
    auto* notificationController = notifications.get();
    javelin::app::DaemonBackgroundController background{services, std::move(notifications)};

    int cacheCommits = 0;
    QEventLoop cacheLoop;
    QObject::connect(&services.mailMutationApplicationService(),
                     &javelin::app::MailMutationApplicationService::cacheCommitted, &cacheLoop,
                     [&cacheCommits, &cacheLoop](const javelin::app::MailCacheChange&)
                     {
                         ++cacheCommits;
                         cacheLoop.quit();
                     });

    REQUIRE(notificationController->notifyNewMail(
        QStringLiteral("account-1"), QStringLiteral("inbox"), QStringLiteral("thread-1"),
        QStringLiteral("email-1"), QStringLiteral("Inbox"), QStringLiteral("New mail in Inbox"),
        QStringLiteral("Background notification")));
    const auto archiveNotificationId = observer->notificationId;
    REQUIRE(QMetaObject::invokeMethod(notificationController, "onActionInvoked",
                                      Qt::DirectConnection, Q_ARG(uint, archiveNotificationId),
                                      Q_ARG(QString, QStringLiteral("archive"))));
    if (cacheCommits == 0)
        cacheLoop.exec();
    REQUIRE(cacheCommits >= 1);

    javelin::jmap::cache::EmailRepository emails{services.databaseConnection()};
    const auto archivedResult = emails.find("account-1", "email-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(archivedResult));
    const auto& archived = std::get<std::optional<javelin::jmap::domain::Email>>(archivedResult);
    REQUIRE(archived.has_value());
    CHECK(std::ranges::contains(archived->mailboxIds, std::string{"archive"}));
    CHECK_FALSE(std::ranges::contains(archived->mailboxIds, std::string{"inbox"}));

    REQUIRE(notificationController->notifyNewMail(
        QStringLiteral("account-1"), QStringLiteral("archive"), QStringLiteral("thread-1"),
        QStringLiteral("email-1"), QStringLiteral("Archive"), QStringLiteral("New mail in Archive"),
        QStringLiteral("Background notification")));
    const auto readNotificationId = observer->notificationId;
    const auto beforeReadCommit = cacheCommits;
    REQUIRE(QMetaObject::invokeMethod(notificationController, "onActionInvoked",
                                      Qt::DirectConnection, Q_ARG(uint, readNotificationId),
                                      Q_ARG(QString, QStringLiteral("mark-read"))));
    if (cacheCommits == beforeReadCommit)
        cacheLoop.exec();
    REQUIRE(cacheCommits > beforeReadCommit);

    const auto readResult = emails.find("account-1", "email-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(readResult));
    const auto& read = std::get<std::optional<javelin::jmap::domain::Email>>(readResult);
    REQUIRE(read.has_value());
    CHECK(std::ranges::contains(read->keywords, std::string{"$seen"}));
    CHECK(services.accountRuntimeManager().configuredAccountIds().empty());
}

TEST_CASE("new mail notification actions emit stable mail intents",
          "[app][daemon][notification][actions]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto transport = std::make_unique<FakeNotificationTransport>();
    auto* observer = transport.get();
    javelin::app::DesktopNotificationController controller{std::move(transport), false, true};

    QString archiveAccount;
    QString archiveMailbox;
    QString archiveEmail;
    QString readAccount;
    QString readEmail;
    QObject::connect(&controller,
                     &javelin::app::DesktopNotificationController::mailArchiveRequested,
                     &controller,
                     [&](const QString& accountId, const QString& mailboxId, const QString& emailId)
                     {
                         archiveAccount = accountId;
                         archiveMailbox = mailboxId;
                         archiveEmail = emailId;
                     });
    QObject::connect(&controller,
                     &javelin::app::DesktopNotificationController::mailMarkReadRequested,
                     &controller,
                     [&](const QString& accountId, const QString& emailId)
                     {
                         readAccount = accountId;
                         readEmail = emailId;
                     });

    REQUIRE(controller.notifyNewMail(QStringLiteral("account-a"), QStringLiteral("inbox-a"),
                                     QStringLiteral("thread-a"), QStringLiteral("email-a"),
                                     QStringLiteral("Inbox"), QStringLiteral("New mail"),
                                     QStringLiteral("First")));
    const auto archiveNotificationId = observer->notificationId;
    REQUIRE(QMetaObject::invokeMethod(&controller, "onActionInvoked", Qt::DirectConnection,
                                      Q_ARG(uint, archiveNotificationId),
                                      Q_ARG(QString, QStringLiteral("archive"))));
    CHECK(archiveAccount == QStringLiteral("account-a"));
    CHECK(archiveMailbox == QStringLiteral("inbox-a"));
    CHECK(archiveEmail == QStringLiteral("email-a"));
    CHECK(observer->closedId == archiveNotificationId);

    REQUIRE(controller.notifyNewMail(QStringLiteral("account-b"), QStringLiteral("inbox-b"),
                                     QStringLiteral("thread-b"), QStringLiteral("email-b"),
                                     QStringLiteral("Inbox"), QStringLiteral("New mail"),
                                     QStringLiteral("Second")));
    const auto readNotificationId = observer->notificationId;
    REQUIRE(QMetaObject::invokeMethod(&controller, "onActionInvoked", Qt::DirectConnection,
                                      Q_ARG(uint, readNotificationId),
                                      Q_ARG(QString, QStringLiteral("mark-read"))));
    CHECK(readAccount == QStringLiteral("account-b"));
    CHECK(readEmail == QStringLiteral("email-b"));
    CHECK(observer->closedId == readNotificationId);
}

TEST_CASE("new mail omits actions when the notification service cannot invoke them",
          "[app][daemon][notification][actions]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto transport = std::make_unique<FakeNotificationTransport>();
    auto* observer = transport.get();
    observer->actionsSupported = false;
    javelin::app::DesktopNotificationController controller{std::move(transport), false, true};

    REQUIRE(controller.notifyNewMail(QStringLiteral("account"), QStringLiteral("inbox"),
                                     QStringLiteral("thread"), QStringLiteral("email"),
                                     QStringLiteral("Inbox"), QStringLiteral("New mail"),
                                     QStringLiteral("Subject")));
    REQUIRE(observer->request.has_value());
    CHECK(observer->request->actions.isEmpty());
    CHECK(observer->request->hints.value(QStringLiteral("desktop-entry")).toString() ==
          QStringLiteral("javelinmail"));
}

TEST_CASE("Flatpak notifications advertise the exported desktop entry",
          "[app][daemon][notification][flatpak]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    const ScopedEnvironmentVariable flatpakId{QByteArrayLiteral("FLATPAK_ID"),
                                              QByteArrayLiteral("app.javelin.JavelinMail")};
    auto transport = std::make_unique<FakeNotificationTransport>();
    auto* observer = transport.get();
    observer->actionsSupported = false;
    javelin::app::DesktopNotificationController controller{std::move(transport), false, true};

    REQUIRE(controller.notifyNewMail(QStringLiteral("account"), QStringLiteral("inbox"),
                                     QStringLiteral("thread"), QStringLiteral("email"),
                                     QStringLiteral("Inbox"), QStringLiteral("New mail"),
                                     QStringLiteral("Subject")));
    REQUIRE(observer->request.has_value());
    CHECK(observer->request->hints.value(QStringLiteral("desktop-entry")).toString() ==
          QStringLiteral("app.javelin.JavelinMail"));
    CHECK(observer->request->hints.value(QStringLiteral("x-kde-appname")).toString() ==
          QStringLiteral("javelinmail"));
}

TEST_CASE("notification action capability is cached until the service restarts",
          "[app][daemon][notification][actions]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto transport = std::make_unique<FakeNotificationTransport>();
    auto* observer = transport.get();
    javelin::app::DesktopNotificationController controller{std::move(transport), false, true};

    REQUIRE(controller.notifyNewMail(QStringLiteral("account"), QStringLiteral("inbox"),
                                     QStringLiteral("thread-1"), QStringLiteral("email-1"),
                                     QStringLiteral("Inbox"), QStringLiteral("New mail"),
                                     QStringLiteral("First")));
    REQUIRE(controller.notifyNewMail(QStringLiteral("account"), QStringLiteral("inbox"),
                                     QStringLiteral("thread-2"), QStringLiteral("email-2"),
                                     QStringLiteral("Inbox"), QStringLiteral("New mail"),
                                     QStringLiteral("Second")));
    CHECK(observer->supportQueryCount == 1);

    REQUIRE(QMetaObject::invokeMethod(
        &controller, "onNotificationServiceUnregistered", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("org.freedesktop.Notifications"))));
    observer->actionsSupported = false;
    REQUIRE(controller.notifyNewMail(QStringLiteral("account"), QStringLiteral("inbox"),
                                     QStringLiteral("thread-3"), QStringLiteral("email-3"),
                                     QStringLiteral("Inbox"), QStringLiteral("New mail"),
                                     QStringLiteral("Third")));
    CHECK(observer->supportQueryCount == 2);
    REQUIRE(observer->request.has_value());
    CHECK(observer->request->actions.isEmpty());
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
    CHECK(observer->request->hints.value(QStringLiteral("x-kde-eventId")).toString() ==
          QStringLiteral("calendar-invitation"));
    CHECK(observer->request->hints.value(QStringLiteral("category")).toString() ==
          QStringLiteral("x-javelin.calendar.invitation"));

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
    CHECK_FALSE(observer->request->hints.contains(QStringLiteral("desktop-entry")));
    CHECK(observer->request->hints.value(QStringLiteral("x-kde-eventId")).toString() ==
          QStringLiteral("undo-send"));
    CHECK(observer->request->hints.value(QStringLiteral("category")).toString() ==
          QStringLiteral("x-javelin.email.undo-send"));
}

TEST_CASE("KDE event settings override installed notification presentation",
          "[app][daemon][notification][settings]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    const auto applicationName = QStringLiteral("javelin-notification-settings-test-%1")
                                     .arg(QCoreApplication::applicationPid());
    const ScopedKdeNotificationConfig config{
        applicationName,
        QByteArrayLiteral("[Event/sound-only]\nAction=Popup|Sound\nSound=message-new-email\n\n"
                          "[Event/disabled]\nAction=Popup|Sound\nSound=dialog-warning\n\n"
                          "[Event/empty-sound]\nAction=Popup|Sound\nSound=message-new-email\n"),
        QByteArrayLiteral("[Event/sound-only]\nAction=Sound\nSound=message-new-instant\n\n"
                          "[Event/disabled]\nAction=None\n\n"
                          "[Event/empty-sound]\nAction=Sound\nSound=\n")};
    const javelin::app::KdeNotificationIntegration integration{
        applicationName, QStringLiteral("Javelin Mail Test"), QStringLiteral("javelinmail"),
        QStringLiteral("javelinmail")};

    const auto soundOnly = integration.presentationFor(QStringLiteral("sound-only"));
    CHECK_FALSE(soundOnly.popup);
    CHECK(soundOnly.sound);
    CHECK(soundOnly.soundName == QStringLiteral("message-new-instant"));

    const auto disabled = integration.presentationFor(QStringLiteral("disabled"));
    CHECK_FALSE(disabled.popup);
    CHECK_FALSE(disabled.sound);

    const auto emptySound = integration.presentationFor(QStringLiteral("empty-sound"));
    CHECK_FALSE(emptySound.popup);
    CHECK(emptySound.sound);
    CHECK(emptySound.soundName.isEmpty());

    const auto missing = integration.presentationFor(QStringLiteral("missing-event"),
                                                     QStringLiteral("message-new-email"));
    CHECK(missing.popup);
    CHECK(missing.sound);
    CHECK(missing.soundName == QStringLiteral("message-new-email"));
}

TEST_CASE("sound-only delivery is accepted for informational notifications but not undo send",
          "[app][daemon][notification][settings]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto transport = std::make_unique<FakeNotificationTransport>();
    auto* observer = transport.get();
    observer->deliverWithoutPopup = true;
    javelin::app::DesktopNotificationController controller{std::move(transport), false, true};

    CHECK(controller.notifyNewMail(QStringLiteral("account"), QStringLiteral("inbox"),
                                   QStringLiteral("thread"), QStringLiteral("email"),
                                   QStringLiteral("Inbox"), QStringLiteral("New mail"),
                                   QStringLiteral("Subject")));
    CHECK_FALSE(controller.notifyUndoableSend(QStringLiteral("send-1"), QStringLiteral("Scheduled"),
                                              QStringLiteral("Subject"), 10'000));
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
