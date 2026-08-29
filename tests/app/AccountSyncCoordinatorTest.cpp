#include "app/account/AccountSyncCoordinator.h"
#include "app/WorkScheduler.h"
#include "app/account/EndpointRetryGate.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/MailboxReadRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/NotificationRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/SyncStateRepository.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
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
        std::size_t transientEmailFailuresRemaining = 0;
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
                if (transientEmailFailuresRemaining > 0)
                {
                    --transientEmailFailuresRemaining;
                    co_return javelin::jmap::api::TransportError{
                        .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
                        .message = "temporary coordinator test failure",
                    };
                }
            }
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

            const bool createsNotification = emailDelta && successfulEmailDeltas == 1;
            const std::string emailState = createsNotification ? "email-state-2" : m_emailState;
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
                    arguments = std::string{R"({"accountId":"account-1","oldState":")"} +
                                m_emailState + R"(","newState":")" + emailState +
                                R"(","hasMoreChanges":false,"created":[)" +
                                (createsNotification ? R"("email-new")" : "") +
                                R"(],"updated":[],"destroyed":[]})";
                }
                else if (name == "Email/get")
                {
                    const bool createdFetch = invocation.callId == "created-emails";
                    const std::string objects =
                        createsNotification && createdFetch
                            ? R"({"id":"email-new","blobId":"blob-new","threadId":"thread-new","mailboxIds":{"inbox":true},"keywords":{},"size":42,"receivedAt":"2026-08-29T00:00:00Z","subject":"New mail","preview":"Preview"})"
                            : "";
                    arguments = std::string{R"({"accountId":"account-1","state":")"} + emailState +
                                R"(","list":[)" + objects + R"(],"notFound":[]})";
                }
                else if (name == "Email/query")
                {
                    m_queryState = "query-state-" + std::to_string(++m_queryGeneration);
                    arguments = std::string{R"({"accountId":"account-1","queryState":")"} +
                                m_queryState +
                                R"(","canCalculateChanges":true,"position":0,"ids":[],"total":0})";
                }
                else if (name == "Email/queryChanges")
                {
                    const auto oldState = m_queryState;
                    m_queryState = "query-state-" + std::to_string(++m_queryGeneration);
                    arguments = std::string{R"({"accountId":"account-1","oldQueryState":")"} +
                                oldState + R"(","newQueryState":")" + m_queryState +
                                R"(","added":[],"removed":[],"hasMoreChanges":false,"total":0})";
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
            if (emailDelta)
            {
                ++successfulEmailDeltas;
                m_emailState = emailState;
            }
            co_return response;
        }

      private:
        std::string m_emailState = "email-state-1";
        std::string m_queryState;
        std::size_t m_queryGeneration = 0;
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
        javelin::app::EndpointRetryGate retryGate;
        javelin::app::AccountSyncCoordinator coordinator;

        CoordinatorFixture()
            : connection(openDatabase()), accounts(connection), mailboxes(connection),
              workScheduler(connection, nullptr, std::chrono::milliseconds{0}),
              retryGate({.initialDelay = std::chrono::milliseconds{1},
                         .maxDelay = std::chrono::milliseconds{1},
                         .probePollInterval = std::chrono::milliseconds{1}}),
              coordinator(connection, transport, networkAccessManager, cooldowns, accounts,
                          mailboxes, workScheduler, retryGate)
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
