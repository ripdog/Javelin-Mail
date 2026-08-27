#include "app/SearchSession.h"
#include "app/MailApplicationEventsPorts.h"
#include "app/MessageListMaterializationPort.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/SearchWindowRepository.h"
#include "jmap/search/EmailSearch.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QCoroFuture>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPromise>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

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
            static char applicationName[] = "javelin-search-session-tests";
            static char* argv[] = {applicationName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    class FakeMailEvents final : public javelin::app::MailApplicationEventsPort
    {
      public:
        using MailApplicationEventsPort::MailApplicationEventsPort;

        [[nodiscard]] std::unordered_map<std::string, javelin::app::MailAccountStatus>
        accountStatuses() const override
        {
            return {};
        }

        void publish(javelin::app::MailCacheInvalidation invalidation)
        {
            Q_EMIT cacheInvalidated(std::move(invalidation));
        }

        void publish(javelin::app::ThreadMaterializationProgress progress)
        {
            Q_EMIT threadMaterializationProgress(std::move(progress));
        }

        void publishStatus(const QString& accountId, const javelin::app::MailAccountStatus status)
        {
            Q_EMIT accountStatusChanged(accountId, status);
        }
    };

    class PendingSearchMaterializationPort final
        : public javelin::app::MessageListMaterializationPort
    {
      public:
        PendingSearchMaterializationPort()
        {
            m_searchPromise.start();
        }

        ~PendingSearchMaterializationPort() override
        {
            if (!m_completed)
            {
                m_searchPromise.addResult(javelin::jmap::OperationError{
                    .message = QStringLiteral("Test search materialization abandoned."),
                });
                m_searchPromise.finish();
            }
        }

        [[nodiscard]] javelin::app::MailboxObservationLease
        beginMailboxObservation(std::string, std::string) override
        {
            return {};
        }

        [[nodiscard]] QCoro::Task<javelin::app::MailboxWindowResult>
        requestMailboxWindow(javelin::app::MailboxWindowIntent) override
        {
            co_return javelin::jmap::OperationError{
                .message = QStringLiteral("Mailbox materialization is not used by this test."),
            };
        }

        [[nodiscard]] QCoro::Task<javelin::app::SearchWindowResult>
        requestSearchWindow(javelin::app::SearchWindowIntent intent) override
        {
            lastSearchIntent = std::move(intent);
            auto result = co_await qCoro(m_searchPromise.future()).takeResult();
            co_return result;
        }

        void retireSearchWindow(std::string, std::string) override
        {
        }

        void ensureThread(javelin::app::ThreadMaterializationIntent intent) override
        {
            ensuredThread = std::move(intent);
        }

        void complete(javelin::app::SearchWindowResult result)
        {
            REQUIRE_FALSE(m_completed);
            m_completed = true;
            m_searchPromise.addResult(std::move(result));
            m_searchPromise.finish();
        }

        std::optional<javelin::app::SearchWindowIntent> lastSearchIntent;
        std::optional<javelin::app::ThreadMaterializationIntent> ensuredThread;

      private:
        QPromise<javelin::app::SearchWindowResult> m_searchPromise;
        bool m_completed = false;
    };

    struct SessionContext
    {
        QTemporaryDir directory;
        javelin::jmap::cache::DatabaseConnection connection;
        QString queries;

        SessionContext(QTemporaryDir temporaryDirectory,
                       javelin::jmap::cache::DatabaseConnection databaseConnection)
            : directory(std::move(temporaryDirectory)), connection(std::move(databaseConnection)),
              queries(connection.database().databaseName())
        {
        }
    };

    [[nodiscard]] SessionContext makeSessionContext(const QString& connectionName)
    {
        QTemporaryDir directory;
        REQUIRE(directory.isValid());
        auto opened = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = connectionName,
            .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
        });
        REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
        return SessionContext{
            std::move(directory),
            std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened)),
        };
    }

    template <typename Predicate> void waitFor(Predicate predicate)
    {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < 2000)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(1);
        }
        REQUIRE(predicate());
    }

    void seedAccount(javelin::jmap::cache::DatabaseConnection& connection)
    {
        QSqlQuery account{connection.database()};
        account.prepare(QStringLiteral(
            "INSERT INTO accounts (account_id, email_address, session_url, is_primary) "
            "VALUES (:account_id, :email_address, :session_url, :is_primary)"));
        account.bindValue(QStringLiteral(":account_id"), QStringLiteral("account-1"));
        account.bindValue(QStringLiteral(":email_address"), QStringLiteral("alice@example.com"));
        account.bindValue(QStringLiteral(":session_url"),
                          QStringLiteral("https://example.test/jmap"));
        account.bindValue(QStringLiteral(":is_primary"), 1);
        REQUIRE(account.exec());
    }

    [[nodiscard]] javelin::jmap::domain::Email email(std::string id, std::string receivedAt)
    {
        const auto threadId = "thread-" + id;
        return {
            .id = std::move(id),
            .blobId = {},
            .threadId = threadId,
            .mailboxIds = {},
            .keywords = {},
            .size = 0,
            .receivedAt = std::move(receivedAt),
            .sentAt = std::nullopt,
            .messageId = {},
            .inReplyTo = {},
            .references = {},
            .hasAttachment = false,
            .subject = std::nullopt,
            .from = {},
            .to = {},
            .cc = {},
            .bcc = {},
            .replyTo = {},
            .preview = std::nullopt,
        };
    }

    [[nodiscard]] std::string searchWindowKey()
    {
        const javelin::jmap::search::EmailSearchCriteria criteria{.from = "sender@example.test"};
        return javelin::jmap::search::cacheKey(criteria, {}) + "|session:test-session";
    }

    void seedSearchData(javelin::jmap::cache::DatabaseConnection& connection,
                        const bool includeSecondWindow)
    {
        seedAccount(connection);
        javelin::jmap::cache::EmailRepository emails{connection};
        REQUIRE_FALSE(emails
                          .replaceAll("account-1", {email("email-1", "2026-08-08T12:00:00Z"),
                                                    email("email-2", "2026-08-08T11:00:00Z"),
                                                    email("email-3", "2026-08-08T10:00:00Z"),
                                                    email("email-4", "2026-08-08T09:00:00Z")})
                          .has_value());

        javelin::jmap::cache::SearchWindowRepository windows{connection};
        REQUIRE_FALSE(windows
                          .replace({
                              .accountId = "account-1",
                              .queryKey = searchWindowKey(),
                              .offset = 0,
                              .limit = 2,
                              .position = 0,
                              .returnedLimit = 2,
                              .total = 4,
                              .queryState = "state-1",
                              .emailIds = {"email-1", "email-2"},
                          })
                          .has_value());
        if (includeSecondWindow)
        {
            REQUIRE_FALSE(windows
                              .replace({
                                  .accountId = "account-1",
                                  .queryKey = searchWindowKey(),
                                  .offset = 2,
                                  .limit = 2,
                                  .position = 2,
                                  .returnedLimit = 2,
                                  .total = 4,
                                  .queryState = "state-1",
                                  .emailIds = {"email-3", "email-4"},
                              })
                              .has_value());
        }
    }

    [[nodiscard]] javelin::app::MailCacheInvalidation
    searchWindowInvalidation(const std::size_t offset, const std::size_t limit)
    {
        return {
            .epoch = 1,
            .changedDomains = {javelin::protocol::ChangedDomain::MailQueryWindows},
            .affectedKeys = {QString::fromStdString(searchWindowKey())},
            .change =
                {
                    .accountId = QStringLiteral("account-1"),
                    .mailboxIds = {},
                    .queryWindows = {},
                    .searchWindows = {{.queryKey = QString::fromStdString(searchWindowKey()),
                                       .offset = offset,
                                       .limit = limit,
                                       .total = 4}},
                    .mailboxTreeChanged = false,
                    .emailObjectsChanged = false,
                    .optimisticProjection = false,
                    .contactsChanged = false,
                },
        };
    }
} // namespace

TEST_CASE("search session raises Thread expansion materialization priority",
          "[app][search-session][thread-coverage]")
{
    ApplicationGuard application;
    auto context = makeSessionContext(QStringLiteral("search-session-thread-test"));
    PendingSearchMaterializationPort materialization;
    FakeMailEvents events;
    javelin::app::SearchSession session{
        "account-1", {.from = "sender@example.test"}, {}, context.queries, materialization, events,
        100,
    };

    session.ensureThreadMaterialized("thread-1");

    REQUIRE(materialization.ensuredThread.has_value());
    CHECK(materialization.ensuredThread->accountId == "account-1");
    CHECK(materialization.ensuredThread->threadId == "thread-1");
}

TEST_CASE("search cache commit terminates its visible refresh", "[app][search-session]")
{
    ApplicationGuard application;
    auto context = makeSessionContext(QStringLiteral("search-session-commit-test"));
    PendingSearchMaterializationPort materialization;
    FakeMailEvents events;
    javelin::app::SearchSession session{
        "account-1", {.from = "sender@example.test"}, {}, context.queries, materialization, events,
        100,
    };

    std::size_t failureCount = 0;
    QObject::connect(&session, &javelin::app::MessageListSession::refreshFailed, &session,
                     [&failureCount](const javelin::jmap::OperationError&) { ++failureCount; });

    session.refresh();
    REQUIRE(session.state().refreshInFlight);
    REQUIRE(materialization.lastSearchIntent.has_value());
    const auto windowKey = materialization.lastSearchIntent->windowKey;

    events.publish({
        .epoch = 1,
        .changedDomains = {javelin::protocol::ChangedDomain::MailQueryWindows},
        .affectedKeys = {QString::fromStdString(windowKey)},
        .change =
            {
                .accountId = QStringLiteral("account-1"),
                .mailboxIds = {},
                .queryWindows = {},
                .searchWindows = {{.queryKey = QString::fromStdString(windowKey),
                                   .offset = 0,
                                   .limit = 100,
                                   .total = 0}},
                .mailboxTreeChanged = false,
                .emailObjectsChanged = false,
                .optimisticProjection = false,
                .contactsChanged = false,
            },
    });

    CHECK_FALSE(session.state().refreshInFlight);

    materialization.complete(javelin::jmap::OperationError{
        .message = QStringLiteral("Late search terminal event must be ignored."),
    });
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

    CHECK_FALSE(session.state().refreshInFlight);
    CHECK(failureCount == 0);
}

TEST_CASE("missing initial online-search cache materializes itself after the cache read",
          "[app][search-session][infinite-scroll]")
{
    ApplicationGuard application;
    auto context = makeSessionContext(QStringLiteral("search-session-cache-miss-test"));
    PendingSearchMaterializationPort materialization;
    FakeMailEvents events;
    javelin::app::SearchSession session{
        "account-1", {.from = "sender@example.test"}, {}, context.queries, materialization, events,
        100,
    };

    CHECK_FALSE(session.state().stale);
    session.loadCachedState();
    waitFor([&] { return materialization.lastSearchIntent.has_value(); });

    REQUIRE(materialization.lastSearchIntent.has_value());
    CHECK(materialization.lastSearchIntent->offset == 0);
    CHECK(materialization.lastSearchIntent->limit == 100);
    CHECK_FALSE(materialization.lastSearchIntent->anchor.has_value());
    CHECK(session.state().refreshInFlight);

    materialization.complete(javelin::jmap::OperationError{
        .message = QStringLiteral("Expected search cache-miss test completion."),
    });
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
}

TEST_CASE("unrelated same-account invalidation does not supersede online search refresh",
          "[app][search-session][cache-race]")
{
    ApplicationGuard application;
    auto context = makeSessionContext(QStringLiteral("search-session-unrelated-test"));
    PendingSearchMaterializationPort materialization;
    FakeMailEvents events;
    javelin::app::SearchSession session{
        "account-1", {.from = "sender@example.test"}, {}, context.queries, materialization, events,
        100,
    };
    std::size_t failureCount = 0;
    QObject::connect(&session, &javelin::app::MessageListSession::refreshFailed, &session,
                     [&failureCount](const javelin::jmap::OperationError&) { ++failureCount; });

    session.refresh();
    REQUIRE(session.state().refreshInFlight);
    events.publish({
        .epoch = 7,
        .changedDomains = {javelin::protocol::ChangedDomain::Calendars},
        .affectedKeys = {QStringLiteral("account-1")},
        .change = {.accountId = QStringLiteral("account-1"),
                   .mailboxIds = {},
                   .queryWindows = {},
                   .searchWindows = {}},
    });
    CHECK(session.state().refreshInFlight);

    materialization.complete(javelin::jmap::OperationError{
        .message = QStringLiteral("Expected search refresh failure."),
    });
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    CHECK_FALSE(session.state().refreshInFlight);
    CHECK(failureCount == 1);
}

TEST_CASE("optimistic metadata invalidation marks an active online search stale",
          "[app][search-session][cache-race][optimistic]")
{
    ApplicationGuard application;
    auto context = makeSessionContext(QStringLiteral("search-session-optimistic-test"));
    PendingSearchMaterializationPort materialization;
    FakeMailEvents events;
    javelin::app::SearchSession session{
        "account-1", {.from = "sender@example.test"}, {}, context.queries, materialization, events,
        100,
    };
    std::size_t failureCount = 0;
    QObject::connect(&session, &javelin::app::MessageListSession::refreshFailed, &session,
                     [&failureCount](const javelin::jmap::OperationError&) { ++failureCount; });

    session.refresh();
    REQUIRE(session.state().refreshInFlight);
    events.publish({
        .epoch = 9,
        .changedDomains = {javelin::protocol::ChangedDomain::MessageMetadata},
        .affectedKeys = {QStringLiteral("account-1")},
        .change = {.accountId = QStringLiteral("account-1"),
                   .mailboxIds = {},
                   .queryWindows = {},
                   .searchWindows = {},
                   .emailObjectsChanged = false,
                   .optimisticProjection = true},
    });
    CHECK(session.state().stale);

    materialization.complete(javelin::jmap::OperationError{
        .message = QStringLiteral("Active refresh failure remains observable."),
    });
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    CHECK(failureCount == 1);
}

TEST_CASE("cached online-search load retries after a superseding same-account invalidation",
          "[app][search-session][cache-race]")
{
    ApplicationGuard application;
    auto context = makeSessionContext(QStringLiteral("search-session-cache-race-test"));
    seedSearchData(context.connection, false);
    PendingSearchMaterializationPort materialization;
    FakeMailEvents events;
    javelin::app::SearchSession session{
        "account-1",
        {.from = "sender@example.test"},
        {},
        context.queries,
        materialization,
        events,
        2,
        javelin::app::RestoredSearchState{
            .mode = javelin::app::SearchMode::Online,
            .sessionId = "test-session",
            .windows = {{.offset = 0, .limit = 2}},
        },
    };

    session.loadCachedState();
    events.publish({
        .epoch = 1,
        .changedDomains = {},
        .affectedKeys = {},
        .change =
            {
                .accountId = QStringLiteral("account-1"),
                .mailboxIds = {},
                .queryWindows = {},
                .searchWindows = {},
            },
    });

    waitFor([&] { return session.state().cacheLoaded; });
    REQUIRE(session.state().items.size() == 2);
    CHECK(session.state().items[0].emailId == "email-1");
    CHECK(session.state().items[1].emailId == "email-2");
}

TEST_CASE("online search restores a loaded infinite-scroll prefix from SQLite windows",
          "[app][search-session][infinite-scroll][persistence]")
{
    ApplicationGuard application;
    auto context = makeSessionContext(QStringLiteral("search-session-restore-prefix-test"));
    seedSearchData(context.connection, true);
    PendingSearchMaterializationPort materialization;
    FakeMailEvents events;
    javelin::app::SearchSession session{
        "account-1",
        {.from = "sender@example.test"},
        {},
        context.queries,
        materialization,
        events,
        2,
        javelin::app::RestoredSearchState{
            .mode = javelin::app::SearchMode::Online,
            .sessionId = "test-session",
            .windows = {{.offset = 0, .limit = 2}, {.offset = 2, .limit = 2}},
        },
    };

    session.loadCachedState();
    waitFor([&] { return session.state().items.size() == 4; });

    CHECK(session.state().cacheLoaded);
    CHECK_FALSE(session.state().stale);
    CHECK(session.state().items[3].emailId == "email-4");
    CHECK(session.windowRequests().size() == 2);
    CHECK_FALSE(materialization.lastSearchIntent.has_value());
}

TEST_CASE("online search infinite scrolling consumes a compatible prefetched bounded window",
          "[app][search-session][infinite-scroll]")
{
    ApplicationGuard application;
    auto context = makeSessionContext(QStringLiteral("search-session-infinite-scroll-test"));
    seedSearchData(context.connection, false);
    PendingSearchMaterializationPort materialization;
    FakeMailEvents events;
    javelin::app::SearchSession session{
        "account-1",
        {.from = "sender@example.test"},
        {},
        context.queries,
        materialization,
        events,
        2,
        javelin::app::RestoredSearchState{
            .mode = javelin::app::SearchMode::Online,
            .sessionId = "test-session",
            .windows = {{.offset = 0, .limit = 2}},
        },
    };

    CHECK_FALSE(session.state().stale);
    session.loadCachedState();
    waitFor([&] { return session.state().cacheLoaded; });
    REQUIRE(session.state().items.size() == 2);
    waitFor([&] { return materialization.lastSearchIntent.has_value(); });
    REQUIRE(materialization.lastSearchIntent.has_value());
    CHECK(materialization.lastSearchIntent->offset == 2);
    CHECK(materialization.lastSearchIntent->limit == 2);
    CHECK_FALSE(materialization.lastSearchIntent->anchor.has_value());
    CHECK(materialization.lastSearchIntent->windowKey == searchWindowKey());

    std::size_t stateChangeCount = 0;
    QObject::connect(&session, &javelin::app::MessageListSession::stateChanged, &session,
                     [&stateChangeCount] { ++stateChangeCount; });
    REQUIRE(session.loadMore());
    CHECK(session.state().loadMoreInFlight);
    const auto loadMoreStateChange = stateChangeCount;
    events.publish(searchWindowInvalidation(0, 2));
    waitFor([&] { return stateChangeCount > loadMoreStateChange; });
    CHECK(session.state().loadMoreInFlight);
    CHECK(session.state().loadMoreError.isEmpty());

    javelin::jmap::cache::SearchWindowRepository windows{context.connection};
    REQUIRE_FALSE(windows
                      .replace({
                          .accountId = "account-1",
                          .queryKey = searchWindowKey(),
                          .offset = 2,
                          .limit = 2,
                          .position = 2,
                          .returnedLimit = 2,
                          .total = 4,
                          .queryState = "state-1",
                          .emailIds = {"email-3", "email-4"},
                      })
                      .has_value());

    materialization.complete(javelin::app::SearchWindowSummary{
        .accountId = "account-1",
        .queryKey = searchWindowKey(),
        .offset = 2,
        .limit = 2,
        .position = 2,
        .returnedLimit = 2,
        .representativeCount = 2,
        .total = 4,
        .queryState = "state-1",
    });

    waitFor([&] { return session.state().items.size() == 4; });
    CHECK_FALSE(session.state().loadMoreInFlight);
    CHECK(session.state().items[0].emailId == "email-1");
    CHECK(session.state().items[3].emailId == "email-4");
    CHECK_FALSE(session.canLoadMore());
}

TEST_CASE("online search exposes visible Thread materialization progress",
          "[app][search-session][thread-coverage][progress]")
{
    ApplicationGuard application;
    auto context = makeSessionContext(QStringLiteral("search-session-thread-progress-test"));
    seedSearchData(context.connection, false);
    PendingSearchMaterializationPort materialization;
    FakeMailEvents events;
    javelin::app::SearchSession session{
        "account-1",
        {.from = "sender@example.test"},
        {},
        context.queries,
        materialization,
        events,
        2,
        javelin::app::RestoredSearchState{
            .mode = javelin::app::SearchMode::Online,
            .sessionId = "test-session",
            .windows = {{.offset = 0, .limit = 2}},
        },
    };

    session.loadCachedState();
    waitFor([&] { return session.state().items.size() == 2; });
    events.publish({.accountId = QStringLiteral("account-1"),
                    .threadIds = {QStringLiteral("thread-email-2")},
                    .inFlight = true,
                    .success = true,
                    .error = {}});
    CHECK(session.state().threadMaterializationInFlight);
    CHECK_FALSE(session.state().refreshInFlight);

    events.publishStatus(QStringLiteral("account-1"),
                         javelin::app::MailAccountStatus::Disconnected);
    CHECK_FALSE(session.state().threadMaterializationInFlight);

    events.publish({.accountId = QStringLiteral("account-1"),
                    .threadIds = {QStringLiteral("thread-email-2")},
                    .inFlight = true,
                    .success = true,
                    .error = {}});
    CHECK(session.state().threadMaterializationInFlight);

    events.publish({.accountId = QStringLiteral("account-1"),
                    .threadIds = {QStringLiteral("thread-email-2")},
                    .inFlight = false,
                    .success = true,
                    .error = {}});
    CHECK_FALSE(session.state().threadMaterializationInFlight);
}
