#include "app/MailQueryApplicationService.h"
#include "app/AccountRuntimeManager.h"
#include "app/ApplicationErrorCoordinator.h"
#include "app/MailboxMaintenanceRegistry.h"
#include "app/WorkScheduler.h"
#include "jmap/AccountBootstrapClient.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/SessionRefreshClient.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/ContactRepository.h"
#include "jmap/cache/MailTagReadRepository.h"
#include "jmap/cache/MailboxFilterReadRepository.h"
#include "jmap/cache/MailboxMessageReadRepository.h"
#include "jmap/cache/MailboxReadRepository.h"
#include "jmap/cache/MailboxStatisticsReadRepository.h"
#include "jmap/cache/SearchWindowRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/query/MailQueryClient.h"
#include "jmap/query/MailQueryMaterializer.h"
#include "jmap/sync/MailboxQueryDescriptor.h"

#include <QCoroTask>
#include <QCoroTimer>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
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
            static char appName[] = "mail-query-application-service-test";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    template <typename Predicate>
    [[nodiscard]] bool waitUntil(Predicate predicate, const int timeoutMilliseconds = 5000)
    {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < timeoutMilliseconds)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        return predicate();
    }

    class RejectingResourceTransport final : public javelin::jmap::api::AbstractTransport
    {
      public:
        [[nodiscard]] QCoro::Task<javelin::jmap::api::TransportResult>
        send(javelin::jmap::api::HttpRequest request) override
        {
            Q_UNUSED(request);
            co_return javelin::jmap::api::TransportError{
                .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
                .message = "Unexpected resource request in mail-query admission test",
            };
        }
    };

    class DelayedQueryTransport final : public javelin::jmap::api::JmapMethodTransport
    {
      public:
        std::size_t calls = 0;
        std::size_t emailQueryCalls = 0;
        std::chrono::milliseconds delay{40};

        [[nodiscard]] QCoro::Task<javelin::jmap::api::JmapMethodTransportResult>
        call(javelin::jmap::api::JmapMethodRequest request) override
        {
            if (request.dispatched)
                request.dispatched();
            const auto callNumber = ++calls;
            for (const auto& invocation : request.envelope.methodCalls)
                if (invocation.name == "Email/query")
                    ++emailQueryCalls;
            if (delay.count() > 0)
            {
                QTimer timer;
                timer.setSingleShot(true);
                timer.start(delay);
                co_await qCoro(timer).waitForTimeout();
            }

            const auto queryState = "query-state-" + std::to_string(callNumber);
            javelin::jmap::api::ResponseEnvelope response;
            response.sessionState = "session-state";
            for (const auto& invocation : request.envelope.methodCalls)
            {
                if (invocation.name == "Email/query")
                {
                    const auto queryPosition =
                        invocation.arguments.find(R"("position":0)") != std::string::npos ? 0 : 100;
                    response.methodResponses.push_back({
                        .name = invocation.name,
                        .arguments = std::string{R"({"accountId":"remote-1","queryState":")"} +
                                     queryState + R"(","canCalculateChanges":true,"position":)" +
                                     std::to_string(queryPosition) +
                                     R"(,"ids":["email-1"],"total":101,"limit":100})",
                        .callId = invocation.callId,
                    });
                    continue;
                }
                if (invocation.name == "Email/get")
                {
                    response.methodResponses.push_back({
                        .name = invocation.name,
                        .arguments =
                            R"({"accountId":"remote-1","state":"email-state-1","list":[{"id":"email-1","blobId":"blob-1","threadId":"thread-1","mailboxIds":{"inbox":true},"keywords":{},"size":42,"receivedAt":"2026-09-05T00:00:00Z","hasAttachment":false,"subject":"Admission test","from":[],"to":[],"cc":[],"bcc":[],"replyTo":[],"preview":"Preview"}],"notFound":[]})",
                        .callId = invocation.callId,
                    });
                    continue;
                }
                co_return javelin::jmap::api::ProtocolError{
                    .code = javelin::jmap::api::ProtocolErrorCode::InvalidRequest,
                    .message = "Unexpected method in mail-query admission test: " + invocation.name,
                };
            }
            co_return response;
        }
    };

    [[nodiscard]] javelin::jmap::api::Session testSession()
    {
        return javelin::jmap::api::Session{
            .username = "user@example.test",
            .apiUrl = "https://example.test/jmap",
            .downloadUrl = {},
            .uploadUrl = {},
            .eventSourceUrl = std::nullopt,
            .state = "session-state",
            .capabilities = {.core = true,
                             .coreDetails =
                                 javelin::jmap::api::CoreCapability{
                                     .maxSizeUpload = std::nullopt,
                                     .maxConcurrentUpload = std::nullopt,
                                     .maxSizeRequest = 1024 * 1024,
                                     .maxConcurrentRequests = 4,
                                     .maxCallsInRequest = 16,
                                     .maxObjectsInGet = 100,
                                     .maxObjectsInSet = 100,
                                     .collationAlgorithms = {},
                                 },
                             .mail = true,
                             .submission = false,
                             .contacts = false,
                             .calendars = false,
                             .sieve = false,
                             .websocket = std::nullopt},
            .accounts = {{"remote-1",
                          {.id = "remote-1",
                           .name = "Personal",
                           .isPersonal = true,
                           .isReadOnly = false,
                           .accountCapabilities = {.mail = true,
                                                   .mailDetails = std::nullopt,
                                                   .submission = std::nullopt,
                                                   .contacts = std::nullopt,
                                                   .calendars = std::nullopt,
                                                   .sieve = false}}}},
            .primaryAccounts = {.mailAccountId = "remote-1",
                                .submissionAccountId = std::nullopt,
                                .contactsAccountId = std::nullopt,
                                .calendarsAccountId = std::nullopt,
                                .sieveAccountId = std::nullopt},
        };
    }

    struct Fixture
    {
        QTemporaryDir directory;
        javelin::jmap::cache::DatabaseConnection database;
        RejectingResourceTransport resourceTransport;
        DelayedQueryTransport methodTransport;
        javelin::jmap::SessionRefreshClient sessionRefresh;
        javelin::jmap::AccountBootstrapClient bootstrap;
        QNetworkAccessManager networkAccessManager;
        javelin::jmap::api::WebSocketFailureCooldowns cooldowns;
        javelin::jmap::cache::AccountRepository accounts;
        javelin::jmap::cache::MailboxReadRepository mailboxes;
        javelin::app::ApplicationErrorCoordinator errors;
        javelin::app::WorkScheduler workScheduler;
        javelin::app::AccountRuntimeManager runtime;
        javelin::jmap::MailQueryClient queryClient;
        javelin::jmap::MailQueryMaterializer materializer;
        javelin::jmap::cache::ContactRepository contacts;
        javelin::jmap::cache::MailTagReadRepository tags;
        javelin::jmap::cache::MailboxStatisticsReadRepository statistics;
        javelin::jmap::cache::MailboxMessageReadRepository messages;
        javelin::jmap::cache::MailboxFilterReadRepository filters;
        javelin::app::MailboxMaintenanceRegistry maintenance;
        javelin::app::MailQueryApplicationService service;

        Fixture()
            : database(openDatabase()), sessionRefresh(database, resourceTransport),
              bootstrap(database, resourceTransport, methodTransport), accounts(database),
              mailboxes(database), errors(accounts),
              workScheduler(database, nullptr, std::chrono::milliseconds{0}),
              runtime(database, sessionRefresh, bootstrap, methodTransport, networkAccessManager,
                      cooldowns, accounts, mailboxes, errors, workScheduler),
              queryClient(database, methodTransport), materializer(database, queryClient),
              contacts(database), tags(database), statistics(database), messages(database),
              filters(database),
              service(database, materializer, methodTransport, accounts, contacts, tags, statistics,
                      messages, filters, runtime, errors, workScheduler, maintenance)
        {
            service.setThreadMaterializationCoordinator(nullptr);
            runtime.setMailQueryRefreshPort(service);
            seedAccount();
            runtime.applySettings({javelin::app::AccountSyncConfiguration{
                .settings = {.connectionId = "connection-1",
                             .revision = 1,
                             .sessionUrl = "https://example.test/.well-known/jmap",
                             .loginEmail = "user@example.test",
                             .apiKey = "token",
                             .refreshToken = {},
                             .tokenEndpoint = {},
                             .oauthClientId = {}},
                .accountId = "account-1",
                .mailboxIds = {},
                .fullSyncMailboxIds = {},
                .notificationMailboxIds = {},
            }});
            javelin::jmap::cache::SessionRepository sessions{database};
            const auto stored =
                sessions.replaceForConnection("connection-1", "remote-1", testSession());
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&stored))
                FAIL(error->message.toStdString());
        }

        ~Fixture()
        {
            runtime.applySettings({});
        }

        [[nodiscard]] javelin::app::MailboxWindowIntent pageIntent(const bool forceRefresh = false)
        {
            return {
                .accountId = "account-1",
                .mailboxId = "inbox",
                .offset = 100,
                .limit = 100,
                .sort = {},
                .forceRefresh = forceRefresh,
                .anchor = std::nullopt,
                .anchorOffset = 1,
            };
        }

        [[nodiscard]] javelin::app::MailboxWindowIntent canonicalIntent()
        {
            auto intent = pageIntent();
            intent.offset = 0;
            return intent;
        }

      private:
        [[nodiscard]] javelin::jmap::cache::DatabaseConnection openDatabase()
        {
            REQUIRE(directory.isValid());
            static int counter = 0;
            auto opened = javelin::jmap::cache::DatabaseConnection::open({
                .connectionName = QStringLiteral("mail-query-admission-%1").arg(++counter),
                .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
            });
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
                FAIL(error->message.toStdString());
            return std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
        }

        void seedAccount()
        {
            QSqlQuery account{database.database()};
            REQUIRE(account.exec(QStringLiteral(
                "INSERT INTO accounts(account_id,connection_id,remote_account_id,email_address,"
                "session_url,is_primary,cap_mail) VALUES('account-1','connection-1','remote-1',"
                "'user@example.test','https://example.test/.well-known/jmap',1,1)")));
        }
    };
} // namespace

TEST_CASE("mail query materialization publishes only semantic background dependencies",
          "[app][mail-query][background-effects]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;

    std::vector<javelin::app::MailCacheChange> changes;
    QObject::connect(&fixture.service, &javelin::app::MailQueryApplicationService::cacheCommitted,
                     &fixture.service, [&changes](javelin::app::MailCacheChange change)
                     { changes.push_back(std::move(change)); });

    std::optional<javelin::app::MailboxWindowResult> firstResult;
    auto first = fixture.service.requestMailboxWindow(fixture.pageIntent(true));
    QCoro::connect(std::move(first), &fixture.service,
                   [&firstResult](javelin::app::MailboxWindowResult result)
                   { firstResult = std::move(result); });
    REQUIRE(waitUntil([&firstResult] { return firstResult.has_value(); }));
    REQUIRE(std::holds_alternative<javelin::app::MailboxWindowSummary>(*firstResult));
    REQUIRE_FALSE(changes.empty());
    const auto& firstChange = changes.back();
    CHECK_FALSE(firstChange.background.offlineCatchUp.accountWide);
    CHECK(firstChange.background.offlineCatchUp.mailboxIds == QStringList{QStringLiteral("inbox")});
    CHECK(firstChange.background.vaultProjectionWorkQueued);

    changes.clear();
    std::optional<javelin::app::MailboxWindowResult> secondResult;
    auto second = fixture.service.requestMailboxWindow(fixture.pageIntent(true));
    QCoro::connect(std::move(second), &fixture.service,
                   [&secondResult](javelin::app::MailboxWindowResult result)
                   { secondResult = std::move(result); });
    REQUIRE(waitUntil([&secondResult] { return secondResult.has_value(); }));
    REQUIRE(std::holds_alternative<javelin::app::MailboxWindowSummary>(*secondResult));
    REQUIRE_FALSE(changes.empty());
    const auto& secondChange = changes.back();
    CHECK_FALSE(secondChange.queryWindows.empty());
    CHECK(secondChange.background.offlineCatchUp.empty());
    CHECK_FALSE(secondChange.background.vaultProjectionWorkQueued);
}

TEST_CASE("mail query admission shares equivalent in-flight mailbox pages",
          "[app][mail-query][admission][single-flight]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;

    std::optional<javelin::app::MailboxWindowResult> firstResult;
    std::optional<javelin::app::MailboxWindowResult> secondResult;
    auto first = fixture.service.requestMailboxWindow(fixture.pageIntent());
    QCoro::connect(std::move(first), &fixture.service,
                   [&firstResult](javelin::app::MailboxWindowResult result)
                   { firstResult = std::move(result); });
    REQUIRE(waitUntil([&fixture] { return fixture.methodTransport.calls == 1; }));

    auto second = fixture.service.requestMailboxWindow(fixture.pageIntent());
    QCoro::connect(std::move(second), &fixture.service,
                   [&secondResult](javelin::app::MailboxWindowResult result)
                   { secondResult = std::move(result); });

    REQUIRE(waitUntil([&] { return firstResult.has_value() && secondResult.has_value(); }));
    CHECK(fixture.methodTransport.calls == 1);
    CHECK(std::holds_alternative<javelin::app::MailboxWindowSummary>(*firstResult));
    CHECK(std::holds_alternative<javelin::app::MailboxWindowSummary>(*secondResult));
}

TEST_CASE("canonical background and foreground mailbox demand share one request",
          "[app][mail-query][admission][canonical]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;

    std::optional<javelin::app::CanonicalMailboxRefreshResult> backgroundResult;
    std::optional<javelin::app::MailboxWindowResult> foregroundResult;
    std::vector<javelin::app::MailCacheChange> changes;
    QObject::connect(&fixture.service, &javelin::app::MailQueryApplicationService::cacheCommitted,
                     &fixture.service, [&changes](javelin::app::MailCacheChange change)
                     { changes.push_back(std::move(change)); });
    auto background = fixture.service.refreshCanonicalMailbox("account-1", "inbox");
    QCoro::connect(std::move(background), &fixture.service,
                   [&backgroundResult](javelin::app::CanonicalMailboxRefreshResult result)
                   { backgroundResult = std::move(result); });
    REQUIRE(waitUntil([&fixture] { return fixture.methodTransport.emailQueryCalls == 1; }));

    auto foreground = fixture.service.requestMailboxWindow(fixture.canonicalIntent());
    QCoro::connect(std::move(foreground), &fixture.service,
                   [&foregroundResult](javelin::app::MailboxWindowResult result)
                   { foregroundResult = std::move(result); });

    REQUIRE(
        waitUntil([&] { return backgroundResult.has_value() && foregroundResult.has_value(); }));
    CHECK(fixture.methodTransport.emailQueryCalls == 1);
    CHECK(std::holds_alternative<javelin::app::CanonicalMailboxRefreshSummary>(*backgroundResult));
    REQUIRE(std::holds_alternative<javelin::app::MailboxWindowSummary>(*foregroundResult));
    REQUIRE(changes.size() == 1);
    REQUIRE(changes.front().queryWindows.size() == 1);
    CHECK(changes.front().queryWindows.front().queryKey ==
          QString::fromStdString(javelin::jmap::sync::mailboxQueryKey({
              .mailboxId = "inbox",
              .sortProperty = "receivedAt",
              .isAscending = false,
              .collapseThreads = true,
          })));
    CHECK(changes.front().queryWindows.front().total == std::optional<std::size_t>{101});
    const auto& summary = std::get<javelin::app::MailboxWindowSummary>(*foregroundResult);
    CHECK(summary.total == std::optional<std::size_t>{101});
    CHECK(summary.representativeCount == 1);
}

TEST_CASE("mail query admission merges explicit refreshes into one follow-up request",
          "[app][mail-query][admission][refresh]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;

    std::optional<javelin::app::MailboxWindowResult> firstResult;
    std::optional<javelin::app::MailboxWindowResult> secondResult;
    std::optional<javelin::app::MailboxWindowResult> thirdResult;
    auto first = fixture.service.requestMailboxWindow(fixture.pageIntent());
    QCoro::connect(std::move(first), &fixture.service,
                   [&firstResult](javelin::app::MailboxWindowResult result)
                   { firstResult = std::move(result); });
    REQUIRE(waitUntil([&fixture] { return fixture.methodTransport.calls == 1; }));

    auto second = fixture.service.requestMailboxWindow(fixture.pageIntent(true));
    QCoro::connect(std::move(second), &fixture.service,
                   [&secondResult](javelin::app::MailboxWindowResult result)
                   { secondResult = std::move(result); });
    auto third = fixture.service.requestMailboxWindow(fixture.pageIntent(true));
    QCoro::connect(std::move(third), &fixture.service,
                   [&thirdResult](javelin::app::MailboxWindowResult result)
                   { thirdResult = std::move(result); });

    REQUIRE(waitUntil(
        [&]
        {
            return firstResult.has_value() && secondResult.has_value() && thirdResult.has_value();
        }));
    CHECK(fixture.methodTransport.calls == 2);
    CHECK(std::holds_alternative<javelin::app::MailboxWindowSummary>(*firstResult));
    CHECK(std::holds_alternative<javelin::app::MailboxWindowSummary>(*secondResult));
    CHECK(std::holds_alternative<javelin::app::MailboxWindowSummary>(*thirdResult));
}

TEST_CASE("retiring a pending search prevents its late window from reappearing",
          "[app][mail-query][admission][search-retirement]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;

    javelin::app::SearchWindowIntent intent;
    intent.accountId = "account-1";
    intent.criteria.text = "needle";
    intent.offset = 0;
    intent.limit = 100;
    intent.sort = {};
    intent.anchor = std::nullopt;
    intent.windowKey = "search-window";

    std::optional<javelin::app::SearchWindowResult> result;
    auto task = fixture.service.requestSearchWindow(intent);
    QCoro::connect(std::move(task), &fixture.service,
                   [&result](javelin::app::SearchWindowResult completed)
                   { result = std::move(completed); });
    REQUIRE(waitUntil([&fixture] { return fixture.methodTransport.emailQueryCalls == 1; }));

    fixture.service.retireSearchWindow("account-1", "search-window");
    REQUIRE(waitUntil([&result] { return result.has_value(); }));
    CHECK(std::holds_alternative<javelin::jmap::OperationError>(*result));

    javelin::jmap::cache::SearchWindowRepository windows{fixture.database};
    const auto found = windows.find("account-1", "search-window", 0, 100);
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::SearchWindowRecord>>(found));
    CHECK_FALSE(
        std::get<std::optional<javelin::jmap::cache::SearchWindowRecord>>(found).has_value());
}
