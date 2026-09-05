#include "app/account/AccountSyncCoordinator.h"
#include "app/MailNotificationService.h"
#include "app/WorkScheduler.h"
#include "app/account/EndpointRetryGate.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxReadRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/NotificationRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/sync/MailboxRefreshExecutor.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
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
            static char appName[] = "javelin-tests";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    [[nodiscard]] bool waitUntil(const std::function<bool()>& predicate,
                                 const int timeoutMilliseconds = 5000)
    {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < timeoutMilliseconds)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        return predicate();
    }

    [[nodiscard]] bool containsMethod(const javelin::jmap::api::JmapMethodRequest& request,
                                      const std::string_view name)
    {
        return std::ranges::any_of(request.envelope.methodCalls,
                                   [name](const auto& call) { return call.name == name; });
    }

    class CoordinatorTransport final : public javelin::jmap::api::JmapMethodTransport
    {
      public:
        javelin::jmap::cache::DatabaseConnection* connection = nullptr;
        std::vector<std::vector<std::string>> requestMethods;
        std::size_t successfulEmailDeltas = 0;
        std::size_t emailDeltaAttempts = 0;
        std::size_t presentationRequests = 0;
        std::size_t emailRebaselineRequests = 0;
        std::size_t transientEmailFailuresRemaining = 0;
        std::size_t recoverableEmailGapsRemaining = 0;
        bool nextEmailDeltaCreatesPagedNotification = false;
        bool failPagedEmailContinuation = false;
        bool notificationCommittedBeforePresentation = false;

        [[nodiscard]] QCoro::Task<javelin::jmap::api::JmapMethodTransportResult>
        call(javelin::jmap::api::JmapMethodRequest request) override
        {
            std::vector<std::string> methods;
            methods.reserve(request.envelope.methodCalls.size());
            for (const auto& invocation : request.envelope.methodCalls)
                methods.push_back(invocation.name);
            requestMethods.push_back(std::move(methods));

            const bool emailDelta = containsMethod(request, "Email/changes");
            const bool presentation = containsMethod(request, "Email/query") ||
                                      containsMethod(request, "Email/queryChanges");
            if (emailDelta)
            {
                ++emailDeltaAttempts;
                if (failPagedEmailContinuation)
                {
                    failPagedEmailContinuation = false;
                    co_return javelin::jmap::api::TransportError{
                        .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
                        .message = "paged continuation failure",
                    };
                }
                if (transientEmailFailuresRemaining > 0)
                {
                    --transientEmailFailuresRemaining;
                    co_return javelin::jmap::api::TransportError{
                        .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
                        .message = "temporary coordinator test failure",
                    };
                }
            }
            const bool pagedNotificationDelta =
                emailDelta && nextEmailDeltaCreatesPagedNotification;
            if (pagedNotificationDelta)
                nextEmailDeltaCreatesPagedNotification = false;
            const bool recoverableEmailGap = emailDelta && recoverableEmailGapsRemaining > 0;
            if (recoverableEmailGap)
                --recoverableEmailGapsRemaining;
            if (presentation)
            {
                ++presentationRequests;
                if (connection != nullptr)
                {
                    QSqlQuery query{connection->database()};
                    if (query.exec(QStringLiteral(
                            "SELECT COUNT(*) FROM mail_notification_event_outbox WHERE "
                            "account_id='account-1' AND email_id='email-new'")) &&
                        query.next() && query.value(0).toInt() == 1)
                    {
                        notificationCommittedBeforePresentation = true;
                    }
                }
            }

            const bool createsNotification = emailDelta && !recoverableEmailGap &&
                                             (pagedNotificationDelta || successfulEmailDeltas == 1);
            const std::string createdEmailId =
                pagedNotificationDelta ? "email-page-one" : "email-new";
            const std::string emailState = pagedNotificationDelta ? "email-state-page-one"
                                           : createsNotification  ? "email-state-2"
                                                                  : m_emailState;
            javelin::jmap::api::ResponseEnvelope response;
            response.sessionState = "session-state";
            for (const auto& invocation : request.envelope.methodCalls)
            {
                std::string name = invocation.name;
                std::string arguments;
                if (name == "Mailbox/changes")
                {
                    arguments =
                        R"({"accountId":"account-1","oldState":"mailbox-state-1","newState":"mailbox-state-1","hasMoreChanges":false,"created":[],"updated":[],"destroyed":[]})";
                }
                else if (name == "Mailbox/get")
                {
                    arguments =
                        R"({"accountId":"account-1","state":"mailbox-state-1","list":[],"notFound":[]})";
                }
                else if (name == "Email/changes")
                {
                    if (recoverableEmailGap)
                    {
                        name = "error";
                        arguments = R"({"type":"cannotCalculateChanges"})";
                    }
                    else
                    {
                        arguments =
                            std::string{R"({"accountId":"account-1","oldState":")"} + m_emailState +
                            R"(","newState":")" + emailState + R"(","hasMoreChanges":)" +
                            (pagedNotificationDelta ? "true" : "false") + R"(,"created":[)" +
                            (createsNotification ? "\"" + createdEmailId + "\"" : "") +
                            R"(],"updated":[],"destroyed":[]})";
                    }
                }
                else if (name == "Email/get")
                {
                    const bool createdFetch = invocation.callId == "created-emails";
                    const bool rebaselineFetch = invocation.callId.starts_with("email-rebaseline-");
                    if (rebaselineFetch)
                    {
                        ++emailRebaselineRequests;
                        m_emailState = "email-state-gap";
                        m_unknownEmailPending = true;
                    }
                    const bool unknownPresentationFetch =
                        m_unknownEmailPending && (invocation.callId == "thread-ids-get" ||
                                                  invocation.callId == "updated-emails-get");
                    const std::string objects =
                        createsNotification && createdFetch
                            ? std::string{R"({"id":")"} + createdEmailId +
                                  R"(","blobId":"blob-new","threadId":"thread-new","mailboxIds":{"inbox":true},"keywords":{},"size":42,"receivedAt":"2026-08-29T00:00:00Z","subject":"New mail","preview":"Preview"})"
                        : unknownPresentationFetch
                            ? R"({"id":"email-gap-new","blobId":"blob-gap-new","threadId":"thread-gap-new","mailboxIds":{"inbox":true},"keywords":{},"size":42,"receivedAt":"2026-08-29T00:00:01Z","subject":"Recovered mail","preview":"Preview"})"
                            : "";
                    const auto& getState = rebaselineFetch ? m_emailState : emailState;
                    arguments = std::string{R"({"accountId":"account-1","state":")"} + getState +
                                R"(","list":[)" + objects + R"(],"notFound":[]})";
                    if (unknownPresentationFetch)
                        m_unknownEmailPending = false;
                }
                else if (name == "Email/query")
                {
                    m_queryState = "query-state-" + std::to_string(++m_queryGeneration);
                    arguments = std::string{R"({"accountId":"account-1","queryState":")"} +
                                m_queryState +
                                R"(","canCalculateChanges":true,"position":0,"ids":[)" +
                                (m_unknownEmailPending ? R"("email-gap-new")" : "") +
                                R"(],"total":)" + (m_unknownEmailPending ? "1" : "0") + "}";
                }
                else if (name == "Email/queryChanges")
                {
                    const auto oldState = m_queryState;
                    m_queryState = "query-state-" + std::to_string(++m_queryGeneration);
                    arguments =
                        std::string{R"({"accountId":"account-1","oldQueryState":")"} + oldState +
                        R"(","newQueryState":")" + m_queryState + R"(","added":[)" +
                        (m_unknownEmailPending ? R"({"id":"email-gap-new","index":0})" : "") +
                        R"(],"removed":[],"hasMoreChanges":false,"total":)" +
                        (m_unknownEmailPending ? "1" : "0") + "}";
                }
                else
                {
                    co_return javelin::jmap::api::ProtocolError{
                        .code = javelin::jmap::api::ProtocolErrorCode::InvalidRequest,
                        .message = "Unexpected coordinator test method: " + name,
                    };
                }
                response.methodResponses.push_back({
                    .name = std::move(name),
                    .arguments = std::move(arguments),
                    .callId = invocation.callId,
                });
            }
            if (emailDelta && !recoverableEmailGap)
            {
                ++successfulEmailDeltas;
                m_emailState = emailState;
                if (pagedNotificationDelta)
                    failPagedEmailContinuation = true;
            }
            co_return response;
        }

      private:
        std::string m_emailState = "email-state-1";
        std::string m_queryState;
        std::size_t m_queryGeneration = 0;
        bool m_unknownEmailPending = false;
    };

    class CoordinatorQueryRefreshPort final : public javelin::app::MailQueryRefreshPort
    {
      public:
        CoordinatorQueryRefreshPort(javelin::jmap::cache::DatabaseConnection& connection,
                                    CoordinatorTransport& transport)
            : m_connection(connection), m_transport(transport)
        {
        }

        [[nodiscard]] QCoro::Task<javelin::app::CanonicalMailboxRefreshResult>
        refreshCanonicalMailbox(std::string accountId, std::string mailboxId) override
        {
            javelin::jmap::api::MethodCaller caller{m_transport};
            const javelin::jmap::api::ApiRequestContext context{
                .credentials =
                    {
                        .accountId = accountId,
                        .emailAddress = "alice@example.com",
                        .sessionUrl = "http://127.0.0.1:9/session",
                        .token = {.accessToken = "token",
                                  .refreshToken = std::nullopt,
                                  .expiry = std::nullopt},
                    },
                .apiUrl = "https://mail.example.test/api",
                .requestLimits =
                    javelin::jmap::api::CoreRequestLimits{
                        .maxSizeRequest = 1000000,
                        .maxConcurrentRequests = 4,
                        .maxCallsInRequest = 16,
                        .maxObjectsInGet = 500,
                        .maxObjectsInSet = 500,
                    },
            };
            javelin::jmap::sync::MailboxRefreshExecutor executor{m_connection, caller, context};
            const auto result = co_await executor.refreshCollapsedMailbox(accountId, mailboxId, {},
                                                                          false, accountId);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                co_return *error;
            const auto& summary = std::get<javelin::jmap::sync::MailboxRefreshSummary>(result);
            if (summary.superseded)
            {
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::Conflict,
                    .message = QStringLiteral("The test mailbox refresh was superseded."),
                };
            }
            co_return javelin::app::CanonicalMailboxRefreshSummary{
                .cacheChanged =
                    summary.canonicalWindowMaterialized || !summary.changedEmailIds.empty(),
            };
        }

      private:
        javelin::jmap::cache::DatabaseConnection& m_connection;
        CoordinatorTransport& m_transport;
    };

    struct CoordinatorFixture
    {
        QTemporaryDir temporaryDirectory;
        javelin::jmap::cache::DatabaseConnection connection;
        CoordinatorTransport transport;
        QNetworkAccessManager networkAccessManager;
        javelin::jmap::api::WebSocketFailureCooldowns cooldowns;
        javelin::jmap::cache::AccountRepository accounts;
        javelin::jmap::cache::MailboxReadRepository mailboxes;
        javelin::app::WorkScheduler workScheduler;
        CoordinatorQueryRefreshPort queryRefreshPort;
        javelin::app::EndpointRetryGate retryGate;
        javelin::app::AccountSyncCoordinator coordinator;

        CoordinatorFixture()
            : connection(openDatabase()), accounts(connection), mailboxes(connection),
              workScheduler(connection, nullptr, std::chrono::milliseconds{0}),
              queryRefreshPort(connection, transport),
              retryGate({.initialDelay = std::chrono::milliseconds{1},
                         .maxDelay = std::chrono::milliseconds{1},
                         .probePollInterval = std::chrono::milliseconds{1}}),
              coordinator(connection, transport, networkAccessManager, cooldowns, accounts,
                          mailboxes, workScheduler, queryRefreshPort, retryGate)
        {
            transport.connection = &connection;
            seed();
            coordinator.applySettings({.connectionId = "account-1",
                                       .revision = 0,
                                       .sessionUrl = "http://127.0.0.1:9/session",
                                       .loginEmail = "alice@example.com",
                                       .apiKey = "token",
                                       .refreshToken = {},
                                       .tokenEndpoint = {},
                                       .oauthClientId = {}},
                                      "account-1", {"inbox"}, {"inbox"});
        }

        ~CoordinatorFixture()
        {
            coordinator.stop();
        }

      private:
        [[nodiscard]] javelin::jmap::cache::DatabaseConnection openDatabase()
        {
            REQUIRE(temporaryDirectory.isValid());
            static int counter = 0;
            auto opened = javelin::jmap::cache::DatabaseConnection::open({
                .connectionName = QStringLiteral("account-sync-coordinator-%1").arg(++counter),
                .databasePath = temporaryDirectory.filePath(QStringLiteral("cache.sqlite3")),
            });
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
                FAIL(error->message.toStdString());
            return std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
        }

        void seed()
        {
            QSqlQuery account{connection.database()};
            REQUIRE(account.exec(QStringLiteral(
                "INSERT INTO accounts(account_id,email_address,session_url,is_primary,cap_mail) "
                "VALUES('account-1','alice@example.com','http://127.0.0.1:9/session',1,1)")));

            javelin::jmap::api::Session session{
                .username = "alice@example.com",
                .apiUrl = "https://mail.example.test/api",
                .downloadUrl = {},
                .uploadUrl = {},
                .eventSourceUrl = "http://127.0.0.1:9/events",
                .state = "session-state",
                .capabilities = {.core = true,
                                 .coreDetails =
                                     javelin::jmap::api::CoreCapability{
                                         .maxSizeUpload = std::nullopt,
                                         .maxConcurrentUpload = std::nullopt,
                                         .maxSizeRequest = 1000000,
                                         .maxConcurrentRequests = 4,
                                         .maxCallsInRequest = 16,
                                         .maxObjectsInGet = 500,
                                         .maxObjectsInSet = 500,
                                         .collationAlgorithms = {},
                                     },
                                 .mail = true,
                                 .submission = false,
                                 .contacts = false,
                                 .calendars = false,
                                 .sieve = false,
                                 .websocket = std::nullopt},
                .accounts = {{"account-1",
                              {.id = "account-1",
                               .name = "Personal",
                               .isPersonal = true,
                               .isReadOnly = false,
                               .accountCapabilities = {.mail = true,
                                                       .mailDetails = std::nullopt,
                                                       .submission = std::nullopt,
                                                       .contacts = std::nullopt,
                                                       .calendars = std::nullopt,
                                                       .sieve = false}}}},
                .primaryAccounts = {.mailAccountId = "account-1",
                                    .submissionAccountId = std::nullopt,
                                    .contactsAccountId = std::nullopt,
                                    .calendarsAccountId = std::nullopt,
                                    .sieveAccountId = std::nullopt},
            };
            javelin::jmap::cache::SessionRepository sessions{connection};
            REQUIRE_FALSE(sessions.replace("account-1", session).has_value());

            javelin::jmap::cache::MailboxRepository mailboxRepository{connection};
            REQUIRE_FALSE(mailboxRepository
                              .replaceAll("account-1", {{.id = "inbox",
                                                         .name = "Inbox",
                                                         .parentId = std::nullopt,
                                                         .role = "inbox",
                                                         .sortOrder = 0,
                                                         .totalEmails = 0,
                                                         .unreadEmails = 0,
                                                         .totalThreads = 0,
                                                         .unreadThreads = 0,
                                                         .isSubscribed = true,
                                                         .myRights = {}}})
                              .has_value());
            javelin::jmap::cache::SyncStateRepository states{connection};
            REQUIRE_FALSE(
                states
                    .upsert({.accountId = "account-1", .objectType = "Mailbox", .queryKey = {}},
                            "mailbox-state-1")
                    .has_value());
            REQUIRE_FALSE(
                states
                    .upsert({.accountId = "account-1", .objectType = "Email", .queryKey = {}},
                            "email-state-1")
                    .has_value());
            javelin::jmap::cache::NotificationRepository notifications{connection};
            REQUIRE_FALSE(notifications.replaceActiveMailboxes("account-1", {"inbox"}).has_value());
        }
    };

} // namespace

TEST_CASE("full coordinator demands reconcile Email before refreshing all mailbox presentations",
          "[app][account][sync][ownership]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    CoordinatorFixture fixture;

    REQUIRE(waitUntil(
        [&fixture]
        {
            return fixture.transport.successfulEmailDeltas >= 1 &&
                   fixture.transport.presentationRequests >= 1;
        }));
    const auto initialPresentationRequests = fixture.transport.presentationRequests;

    REQUIRE(fixture.coordinator.requestSynchronization());
    REQUIRE(waitUntil(
        [&fixture, initialPresentationRequests]
        {
            return fixture.transport.successfulEmailDeltas >= 2 &&
                   fixture.transport.presentationRequests > initialPresentationRequests;
        }));

    CHECK(fixture.transport.notificationCommittedBeforePresentation);
    javelin::jmap::cache::SyncStateRepository states{fixture.connection};
    const auto emailState =
        states.find({.accountId = "account-1", .objectType = "Email", .queryKey = {}});
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(emailState));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(emailState).has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(emailState)->stateToken ==
          "email-state-2");
}

TEST_CASE("committed Email delta page publishes before a failed continuation",
          "[app][account][sync][publication][notification][retry]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    CoordinatorFixture fixture;
    REQUIRE(waitUntil(
        [&fixture]
        {
            return fixture.transport.successfulEmailDeltas >= 1 &&
                   fixture.transport.presentationRequests >= 1;
        }));

    javelin::app::MailNotificationService notifications{fixture.connection};
    QObject::connect(&fixture.coordinator,
                     &javelin::app::AccountSyncCoordinator::notificationEventsCommitted,
                     &notifications, &javelin::app::MailNotificationService::accountChanged);
    int deliveries = 0;
    QObject::connect(
        &notifications, &javelin::app::MailNotificationService::notificationRaised,
        [&notifications, &deliveries](const QString& accountId, const QString&, const QString&,
                                      const QString&, const QString&, const QString&,
                                      const QString&, const QStringList& deliveredEmailIds)
        {
            ++deliveries;
            REQUIRE_FALSE(notifications.markDelivered(accountId.toStdString(), deliveredEmailIds)
                              .has_value());
        });

    int emailPublications = 0;
    QObject::connect(&fixture.coordinator, &javelin::app::AccountSyncCoordinator::cacheCommitted,
                     [&emailPublications](const javelin::app::MailCacheChange& change)
                     {
                         if (change.emailObjectsChanged)
                             ++emailPublications;
                     });

    const auto attemptsBefore = fixture.transport.emailDeltaAttempts;
    fixture.transport.nextEmailDeltaCreatesPagedNotification = true;
    REQUIRE(fixture.coordinator.requestSynchronization());
    REQUIRE(waitUntil(
        [&fixture, attemptsBefore, &deliveries, &emailPublications]
        {
            return fixture.transport.emailDeltaAttempts >= attemptsBefore + 2 && deliveries == 1 &&
                   emailPublications >= 1;
        }));

    javelin::jmap::cache::SyncStateRepository states{fixture.connection};
    const auto committedState =
        states.find({.accountId = "account-1", .objectType = "Email", .queryKey = {}});
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(
        committedState));
    REQUIRE(
        std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(committedState).has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(committedState)
              ->stateToken == "email-state-page-one");

    REQUIRE(waitUntil([&fixture, attemptsBefore]
                      { return fixture.transport.emailDeltaAttempts >= attemptsBefore + 3; }));
    CHECK(deliveries == 1);

    javelin::jmap::cache::NotificationRepository repository{fixture.connection};
    const auto pending = repository.listPendingEvents("account-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
        pending));
    CHECK(
        std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(pending).empty());
}

TEST_CASE("continuous mail pushes cannot postpone the first debounce deadline",
          "[app][account][sync][debounce]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    CoordinatorFixture fixture;
    REQUIRE(waitUntil(
        [&fixture]
        {
            return fixture.transport.successfulEmailDeltas >= 1 &&
                   fixture.transport.presentationRequests >= 1;
        }));

    const auto attemptsBefore = fixture.transport.emailDeltaAttempts;
    QElapsedTimer burst;
    burst.start();
    for (int index = 0; index < 10; ++index)
    {
        QCoro::waitFor(fixture.coordinator.onStateChange({
            .newState = "push-burst-" + std::to_string(index),
            .changedTypes = {"Email"},
            .changedStates = {{"account-1", {{"Email", "email-burst-state"}}}},
        }));

        QElapsedTimer spacing;
        spacing.start();
        while (spacing.elapsed() < 100)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }

    CHECK(burst.elapsed() < 1300);
    CHECK(fixture.transport.emailDeltaAttempts > attemptsBefore);
}

TEST_CASE("reconnect and transient retry keep the authoritative Email reconciliation demand",
          "[app][account][sync][ownership][retry]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    CoordinatorFixture fixture;
    REQUIRE(waitUntil(
        [&fixture]
        {
            return fixture.transport.successfulEmailDeltas >= 1 &&
                   fixture.transport.presentationRequests >= 1;
        }));

    const auto beforeReconnect = fixture.transport.successfulEmailDeltas;
    const auto presentationsBeforeReconnect = fixture.transport.presentationRequests;
    fixture.coordinator.networkBecameReachable();
    REQUIRE(waitUntil(
        [&fixture, beforeReconnect, presentationsBeforeReconnect]
        {
            return fixture.transport.successfulEmailDeltas > beforeReconnect &&
                   fixture.transport.presentationRequests > presentationsBeforeReconnect;
        }));

    const auto beforeRetry = fixture.transport.successfulEmailDeltas;
    const auto attemptsBeforeRetry = fixture.transport.emailDeltaAttempts;
    fixture.transport.transientEmailFailuresRemaining = 1;
    REQUIRE(fixture.coordinator.requestSynchronization());
    REQUIRE(waitUntil(
        [&fixture, beforeRetry, attemptsBeforeRetry]
        {
            return fixture.transport.emailDeltaAttempts >= attemptsBeforeRetry + 2 &&
                   fixture.transport.successfulEmailDeltas > beforeRetry;
        }));
}

TEST_CASE("notification baseline execution has one coordinator retry path",
          "[app][account][sync][notification][baseline][retry]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    CoordinatorFixture fixture;
    REQUIRE(waitUntil(
        [&fixture]
        {
            return fixture.transport.successfulEmailDeltas >= 1 &&
                   fixture.transport.presentationRequests >= 1;
        }));

    const auto attemptsBeforeBaseline = fixture.transport.emailDeltaAttempts;
    const auto successesBeforeBaseline = fixture.transport.successfulEmailDeltas;
    fixture.transport.transientEmailFailuresRemaining = 1;
    REQUIRE_FALSE(fixture.coordinator.requestNotificationBaseline({"inbox"}).has_value());
    REQUIRE(fixture.coordinator.requestSynchronization());
    REQUIRE(waitUntil([&fixture, attemptsBeforeBaseline]
                      { return fixture.transport.emailDeltaAttempts > attemptsBeforeBaseline; }));

    QElapsedTimer immediateRetryWindow;
    immediateRetryWindow.start();
    while (immediateRetryWindow.elapsed() < 100)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    CHECK(fixture.transport.emailDeltaAttempts == attemptsBeforeBaseline + 1);

    REQUIRE(
        waitUntil([&fixture, successesBeforeBaseline]
                  { return fixture.transport.successfulEmailDeltas > successesBeforeBaseline; }));
}

TEST_CASE("calendar alert push routes independently of collection state",
          "[app][account][sync][calendar][alert]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    CoordinatorFixture fixture;

    int deliveries = 0;
    QString ownerAccountId;
    QString calendarAccountId;
    QString eventId;
    QString recurrenceId;
    QString alertId;
    QObject::connect(&fixture.coordinator,
                     &javelin::app::AccountSyncCoordinator::calendarAlertReceived,
                     [&deliveries, &ownerAccountId, &calendarAccountId, &eventId, &recurrenceId,
                      &alertId](const QString& owner, const QString& account, const QString& event,
                                const QString&, const QString& recurrence, const QString& alert)
                     {
                         ++deliveries;
                         ownerAccountId = owner;
                         calendarAccountId = account;
                         eventId = event;
                         recurrenceId = recurrence;
                         alertId = alert;
                     });

    QCoro::waitFor(fixture.coordinator.onCalendarAlert({
        .accountId = "calendar-account",
        .calendarEventId = "event-1",
        .uid = "uid-1",
        .recurrenceId = std::string{"2026-09-03T09:00:00"},
        .alertId = "alert-1",
    }));

    CHECK(deliveries == 1);
    CHECK(ownerAccountId == QStringLiteral("account-1"));
    CHECK(calendarAccountId == QStringLiteral("calendar-account"));
    CHECK(eventId == QStringLiteral("event-1"));
    CHECK(recurrenceId == QStringLiteral("2026-09-03T09:00:00"));
    CHECK(alertId == QStringLiteral("alert-1"));
}

TEST_CASE("lost account Email history reconciles every tracked mailbox query",
          "[app][account][sync][ownership][rebaseline]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    CoordinatorFixture fixture;
    REQUIRE(waitUntil(
        [&fixture]
        {
            return fixture.transport.successfulEmailDeltas >= 1 &&
                   fixture.transport.presentationRequests >= 1;
        }));

    const auto presentationsBeforeGap = fixture.transport.presentationRequests;
    fixture.transport.recoverableEmailGapsRemaining = 1;
    QCoro::waitFor(fixture.coordinator.onStateChange({
        .newState = "push-state-gap",
        .changedTypes = {"Email"},
        .changedStates = {{"account-1", {{"Email", "email-state-gap"}}}},
    }));

    REQUIRE(waitUntil(
        [&fixture, presentationsBeforeGap]
        {
            return fixture.transport.emailRebaselineRequests == 1 &&
                   fixture.transport.presentationRequests > presentationsBeforeGap;
        }));
    CHECK(fixture.transport.emailDeltaAttempts >= 2);
    javelin::jmap::cache::EmailRepository emails{fixture.connection};
    const auto recovered = emails.find("account-1", "email-gap-new");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(recovered));
    CHECK(std::get<std::optional<javelin::jmap::domain::Email>>(recovered).has_value());
}
