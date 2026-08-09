#include "app/MailboxSession.h"
#include "app/MailApplicationEventsPorts.h"
#include "app/MessageListMaterializationPort.h"
#include "jmap/cache/Database.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/QueryService.h"
#include "jmap/cache/SearchWindowRepository.h"
#include "jmap/sync/MailboxQueryDescriptor.h"

#include <QCoroFuture>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPromise>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
            static char applicationName[] = "javelin-mailbox-session-tests";
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
    };

    class PendingMaterializationPort final : public javelin::app::MessageListMaterializationPort
    {
      public:
        PendingMaterializationPort()
        {
            m_mailboxPromise.start();
        }

        ~PendingMaterializationPort() override
        {
            if (!m_completed)
            {
                m_mailboxPromise.addResult(javelin::jmap::OperationError{
                    .message = QStringLiteral("Test materialization abandoned."),
                });
                m_mailboxPromise.finish();
            }
        }

        [[nodiscard]] javelin::app::MailboxObservationLease
        beginMailboxObservation(std::string, std::string) override
        {
            return {};
        }

        [[nodiscard]] QCoro::Task<javelin::app::MailboxWindowResult>
        requestMailboxWindow(javelin::app::MailboxWindowIntent intent) override
        {
            lastMailboxIntent = std::move(intent);
            auto result = co_await qCoro(m_mailboxPromise.future()).takeResult();
            co_return result;
        }

        [[nodiscard]] QCoro::Task<javelin::app::SearchWindowResult>
        requestSearchWindow(javelin::app::SearchWindowIntent intent) override
        {
            lastSearchIntent = std::move(intent);
            co_return javelin::jmap::OperationError{
                .message = QStringLiteral("Expected test search materialization completion."),
            };
        }

        void retireSearchWindow(std::string accountId, std::string windowKey) override
        {
            retiredSearchWindow = std::pair{std::move(accountId), std::move(windowKey)};
        }

        void complete(javelin::app::MailboxWindowResult result)
        {
            REQUIRE_FALSE(m_completed);
            m_completed = true;
            m_mailboxPromise.addResult(std::move(result));
            m_mailboxPromise.finish();
        }

        std::optional<javelin::app::MailboxWindowIntent> lastMailboxIntent;
        std::optional<javelin::app::SearchWindowIntent> lastSearchIntent;
        std::optional<std::pair<std::string, std::string>> retiredSearchWindow;

      private:
        QPromise<javelin::app::MailboxWindowResult> m_mailboxPromise;
        bool m_completed = false;
    };

    struct SessionContext
    {
        QTemporaryDir directory;
        javelin::jmap::cache::DatabaseConnection connection;
        javelin::jmap::cache::QueryService queries;

        SessionContext(QTemporaryDir temporaryDirectory,
                       javelin::jmap::cache::DatabaseConnection databaseConnection)
            : directory(std::move(temporaryDirectory)), connection(std::move(databaseConnection)),
              queries(connection)
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

    void seedAccountAndMailbox(javelin::jmap::cache::DatabaseConnection& connection)
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

        javelin::jmap::cache::MailboxRepository mailboxes{connection};
        REQUIRE_FALSE(mailboxes
                          .replaceAll("account-1", {{.id = "mailbox-1",
                                                     .name = "Inbox",
                                                     .parentId = std::nullopt,
                                                     .role = "inbox",
                                                     .sortOrder = 0,
                                                     .totalEmails = 4,
                                                     .unreadEmails = 0,
                                                     .totalThreads = 4,
                                                     .unreadThreads = 0,
                                                     .isSubscribed = true,
                                                     .myRights = {}}})
                          .has_value());
    }

    [[nodiscard]] javelin::jmap::domain::Email email(std::string id, std::string receivedAt)
    {
        const auto threadId = "thread-" + id;
        return {
            .id = std::move(id),
            .blobId = {},
            .threadId = threadId,
            .mailboxIds = {"mailbox-1"},
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

    [[nodiscard]] std::string mailboxQueryKey()
    {
        return javelin::jmap::sync::mailboxQueryKey({
            .mailboxId = "mailbox-1",
            .sortProperty = "receivedAt",
            .isAscending = false,
            .collapseThreads = true,
        });
    }

    void seedInfiniteScrollData(javelin::jmap::cache::DatabaseConnection& connection,
                                const bool includeSecondWindow)
    {
        seedAccountAndMailbox(connection);
        javelin::jmap::cache::EmailRepository emails{connection};
        REQUIRE_FALSE(emails
                          .replaceAll("account-1", {email("email-1", "2026-08-08T12:00:00Z"),
                                                    email("email-2", "2026-08-08T11:00:00Z"),
                                                    email("email-3", "2026-08-08T10:00:00Z"),
                                                    email("email-4", "2026-08-08T09:00:00Z")})
                          .has_value());

        javelin::jmap::cache::MailboxWindowRepository windows{connection};
        REQUIRE_FALSE(windows
                          .replace({
                              .accountId = "account-1",
                              .mailboxId = "mailbox-1",
                              .queryKey = mailboxQueryKey(),
                              .requestedOffset = 0,
                              .requestedLimit = 2,
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
                                  .mailboxId = "mailbox-1",
                                  .queryKey = mailboxQueryKey(),
                                  .requestedOffset = 2,
                                  .requestedLimit = 2,
                                  .position = 2,
                                  .returnedLimit = 2,
                                  .total = 4,
                                  .queryState = "state-1",
                                  .emailIds = {"email-3", "email-4"},
                              })
                              .has_value());
        }
    }

    [[nodiscard]] javelin::app::MailCacheInvalidation windowInvalidation(const std::size_t offset,
                                                                         const std::size_t limit)
    {
        return {
            .epoch = 1,
            .changedDomains = {javelin::protocol::ChangedDomain::MailQueryWindows},
            .affectedKeys = {QStringLiteral("mailbox-1")},
            .change =
                {
                    .accountId = QStringLiteral("account-1"),
                    .mailboxIds = {},
                    .queryWindows = {{.mailboxId = QStringLiteral("mailbox-1"),
                                      .offset = offset,
                                      .limit = limit,
                                      .total = 4}},
                    .searchWindows = {},
                    .mailboxTreeChanged = false,
                    .hasNewMail = false,
                    .optimisticProjection = false,
                    .contactsChanged = false,
                },
        };
    }

    [[nodiscard]] javelin::app::MailCacheInvalidation
    searchWindowInvalidation(const std::string& queryKey, const std::size_t total,
                             const std::uint64_t epoch, const std::size_t limit = 100)
    {
        return {
            .epoch = epoch,
            .changedDomains = {javelin::protocol::ChangedDomain::MailQueryWindows},
            .affectedKeys = {QString::fromStdString(queryKey)},
            .change =
                {
                    .accountId = QStringLiteral("account-1"),
                    .mailboxIds = {},
                    .queryWindows = {},
                    .searchWindows = {{.queryKey = QString::fromStdString(queryKey),
                                       .offset = 0,
                                       .limit = limit,
                                       .total = total}},
                    .mailboxTreeChanged = false,
                    .hasNewMail = false,
                    .optimisticProjection = false,
                    .contactsChanged = false,
                },
        };
    }
} // namespace

TEST_CASE("mailbox cache commit terminates its visible refresh", "[app][mailbox-session]")
{
    ApplicationGuard application;
    auto context = makeSessionContext(QStringLiteral("mailbox-session-commit-test"));
    PendingMaterializationPort materialization;
    FakeMailEvents events;
    javelin::app::MailboxSession session{
        "account-1", "mailbox-1",     QStringLiteral("Inbox"), std::optional<std::string>{"inbox"},
        {},          context.queries, materialization,         100,
        events};

    std::size_t failureCount = 0;
    QObject::connect(&session, &javelin::app::MessageListSession::refreshFailed, &session,
                     [&failureCount](const javelin::jmap::OperationError&) { ++failureCount; });

    session.markStale();
    session.refresh();
    REQUIRE(session.state().refreshInFlight);
    REQUIRE(materialization.lastMailboxIntent.has_value());
    CHECK_FALSE(materialization.lastMailboxIntent->forceRefresh);

    events.publish(windowInvalidation(0, 100));
    CHECK_FALSE(session.state().refreshInFlight);

    materialization.complete(javelin::jmap::OperationError{
        .message = QStringLiteral("Late terminal event must be ignored."),
    });
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

    CHECK_FALSE(session.state().refreshInFlight);
    CHECK(failureCount == 0);
}

TEST_CASE("missing initial mailbox cache materializes itself after the cache read",
          "[app][mailbox-session][infinite-scroll]")
{
    ApplicationGuard application;
    auto context = makeSessionContext(QStringLiteral("mailbox-session-cache-miss-test"));
    PendingMaterializationPort materialization;
    FakeMailEvents events;
    javelin::app::MailboxSession session{
        "account-1", "mailbox-1",     QStringLiteral("Inbox"), std::optional<std::string>{"inbox"},
        {},          context.queries, materialization,         100,
        events};

    CHECK_FALSE(session.state().stale);
    session.loadCachedState();
    waitFor([&] { return materialization.lastMailboxIntent.has_value(); });

    REQUIRE(materialization.lastMailboxIntent.has_value());
    CHECK(materialization.lastMailboxIntent->offset == 0);
    CHECK(materialization.lastMailboxIntent->limit == 100);
    CHECK_FALSE(materialization.lastMailboxIntent->anchor.has_value());
    CHECK_FALSE(materialization.lastMailboxIntent->forceRefresh);
    CHECK(session.state().refreshInFlight);

    materialization.complete(javelin::jmap::OperationError{
        .message = QStringLiteral("Expected cache-miss test completion."),
    });
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
}

TEST_CASE("explicit mailbox refresh requests server reconciliation", "[app][mailbox-session]")
{
    ApplicationGuard application;
    auto context = makeSessionContext(QStringLiteral("mailbox-session-explicit-refresh-test"));
    PendingMaterializationPort materialization;
    FakeMailEvents events;
    javelin::app::MailboxSession session{
        "account-1", "mailbox-1",     QStringLiteral("Inbox"), std::optional<std::string>{"inbox"},
        {},          context.queries, materialization,         100,
        events};

    session.refresh(javelin::app::MessageListRefreshMode::RefreshFromServer);
    REQUIRE(materialization.lastMailboxIntent.has_value());
    CHECK(materialization.lastMailboxIntent->forceRefresh);

    materialization.complete(javelin::jmap::OperationError{
        .message = QStringLiteral("Expected explicit refresh test completion."),
    });
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
}

TEST_CASE("mailbox quick filter stays inline and requests a filtered mailbox window",
          "[app][mailbox-session][quick-filter]")
{
    ApplicationGuard application;
    auto context = makeSessionContext(QStringLiteral("mailbox-session-quick-filter-test"));
    PendingMaterializationPort materialization;
    FakeMailEvents events;
    javelin::app::MailboxSession session{
        "account-1", "mailbox-1",     QStringLiteral("Inbox"), std::optional<std::string>{"inbox"},
        {},          context.queries, materialization,         100,
        events};

    session.setQuickFilter({.unreadOnly = true, .hasAttachmentOnly = true});
    waitFor([&] { return materialization.lastSearchIntent.has_value(); });

    REQUIRE(materialization.lastSearchIntent.has_value());
    CHECK_FALSE(materialization.lastMailboxIntent.has_value());
    CHECK(materialization.lastSearchIntent->accountId == "account-1");
    CHECK(materialization.lastSearchIntent->criteria.inMailbox ==
          std::optional<std::string>{"mailbox-1"});
    CHECK(materialization.lastSearchIntent->criteria.unreadOnly);
    CHECK(materialization.lastSearchIntent->criteria.hasAttachmentOnly);
    CHECK(materialization.lastSearchIntent->windowKey.starts_with("quick-filter:"));
    CHECK(session.quickFilterActive());
    CHECK(session.windowRequests().empty());
}

TEST_CASE("quick filter keeps only the selected nonmatching message as continuity",
          "[app][mailbox-session][quick-filter]")
{
    ApplicationGuard application;
    auto context =
        makeSessionContext(QStringLiteral("mailbox-session-quick-filter-continuity-test"));
    seedAccountAndMailbox(context.connection);
    javelin::jmap::cache::EmailRepository emails{context.connection};
    auto first = email("email-1", "2026-08-08T12:00:00Z");
    auto second = email("email-2", "2026-08-08T11:00:00Z");
    REQUIRE_FALSE(emails.replaceAll("account-1", {first, second}).has_value());
    PendingMaterializationPort materialization;
    FakeMailEvents events;
    javelin::app::MailboxSession session{
        "account-1", "mailbox-1",     QStringLiteral("Inbox"), std::optional<std::string>{"inbox"},
        {},          context.queries, materialization,         100,
        events};

    session.setQuickFilter({.unreadOnly = true});
    waitFor([&] { return materialization.lastSearchIntent.has_value(); });
    const auto queryKey = materialization.lastSearchIntent->windowKey;

    javelin::jmap::cache::SearchWindowRepository windows{context.connection};
    REQUIRE_FALSE(windows
                      .replace({
                          .accountId = "account-1",
                          .queryKey = queryKey,
                          .offset = 0,
                          .limit = 100,
                          .position = 0,
                          .returnedLimit = 100,
                          .total = 2,
                          .queryState = "state-1",
                          .emailIds = {"email-1", "email-2"},
                      })
                      .has_value());
    events.publish(searchWindowInvalidation(queryKey, 2, 1));
    waitFor([&] { return session.state().items.size() == 2; });

    session.setQuickFilterContinuitySelection("email-1", "thread-email-1");
    first.keywords = {"$seen"};
    REQUIRE_FALSE(emails.upsertMany("account-1", {first}).has_value());
    REQUIRE_FALSE(windows
                      .replace({
                          .accountId = "account-1",
                          .queryKey = queryKey,
                          .offset = 0,
                          .limit = 100,
                          .position = 0,
                          .returnedLimit = 100,
                          .total = 1,
                          .queryState = "state-2",
                          .emailIds = {"email-2"},
                      })
                      .has_value());
    events.publish(searchWindowInvalidation(queryKey, 1, 2));
    waitFor(
        [&]
        {
            const auto selected =
                std::ranges::find(session.state().items, std::string{"email-1"},
                                  &javelin::jmap::cache::MessageListItem::emailId);
            return selected != session.state().items.end() && !selected->isUnread;
        });

    REQUIRE(session.state().total == std::optional<std::size_t>{1});
    REQUIRE(session.state().items.size() == 2);
    CHECK(session.state().items[0].emailId == "email-1");
    CHECK_FALSE(session.state().items[0].isUnread);
    CHECK(session.state().items[1].emailId == "email-2");

    session.setQuickFilterContinuitySelection("email-2", "thread-email-2");
    waitFor([&] { return session.state().items.size() == 1; });
    CHECK(session.state().items[0].emailId == "email-2");
    CHECK(session.state().total == std::optional<std::size_t>{1});
}

TEST_CASE("quick filter preserves the selected email when a thread representative changes",
          "[app][mailbox-session][quick-filter]")
{
    ApplicationGuard application;
    auto context = makeSessionContext(
        QStringLiteral("mailbox-session-quick-filter-representative-continuity-test"));
    seedAccountAndMailbox(context.connection);
    javelin::jmap::cache::EmailRepository emails{context.connection};
    auto selected = email("email-1", "2026-08-08T12:00:00Z");
    auto replacement = email("email-1-reply", "2026-08-08T11:30:00Z");
    replacement.threadId = selected.threadId;
    auto other = email("email-2", "2026-08-08T11:00:00Z");
    REQUIRE_FALSE(emails.replaceAll("account-1", {selected, replacement, other}).has_value());
    PendingMaterializationPort materialization;
    FakeMailEvents events;
    javelin::app::MailboxSession session{
        "account-1", "mailbox-1",     QStringLiteral("Inbox"), std::optional<std::string>{"inbox"},
        {},          context.queries, materialization,         2,
        events};

    session.setQuickFilter({.unreadOnly = true});
    waitFor([&] { return materialization.lastSearchIntent.has_value(); });
    const auto queryKey = materialization.lastSearchIntent->windowKey;

    javelin::jmap::cache::SearchWindowRepository windows{context.connection};
    REQUIRE_FALSE(windows
                      .replace({
                          .accountId = "account-1",
                          .queryKey = queryKey,
                          .offset = 0,
                          .limit = 2,
                          .position = 0,
                          .returnedLimit = 2,
                          .total = 3,
                          .queryState = "state-1",
                          .emailIds = {"email-2", "email-1"},
                      })
                      .has_value());
    events.publish(searchWindowInvalidation(queryKey, 3, 1, 2));
    waitFor([&] { return session.state().items.size() == 2; });

    session.setQuickFilterContinuitySelection(selected.id, selected.threadId);
    selected.keywords = {"$seen"};
    REQUIRE_FALSE(emails.upsertMany("account-1", {selected}).has_value());
    REQUIRE_FALSE(windows
                      .replace({
                          .accountId = "account-1",
                          .queryKey = queryKey,
                          .offset = 0,
                          .limit = 2,
                          .position = 0,
                          .returnedLimit = 2,
                          .total = 3,
                          .queryState = "state-2",
                          .emailIds = {"email-2", "email-1-reply"},
                      })
                      .has_value());
    const auto beforeRepresentativeChange = session.state().itemsRevision;
    events.publish(searchWindowInvalidation(queryKey, 3, 2, 2));
    waitFor([&] { return session.state().itemsRevision > beforeRepresentativeChange; });

    REQUIRE(session.state().items.size() == 2);
    CHECK(session.state().items[0].emailId == "email-2");
    CHECK(session.state().items[1].emailId == "email-1");
    CHECK_FALSE(session.state().items[1].isUnread);
    CHECK(session.state().items[1].threadMessageCount == 2);
    CHECK(session.state().total == std::optional<std::size_t>{3});

    REQUIRE(session.loadMore());
    REQUIRE(materialization.lastSearchIntent.has_value());
    CHECK(materialization.lastSearchIntent->offset == 2);
    CHECK(materialization.lastSearchIntent->anchor == std::optional<std::string>{"email-1-reply"});

    session.setQuickFilterContinuitySelection(other.id, other.threadId);
    waitFor(
        [&]
        {
            return session.state().items.size() == 2 &&
                   session.state().items[1].emailId == "email-1-reply";
        });
}

TEST_CASE("changing mailbox sort invalidates an obsolete refresh completion",
          "[app][mailbox-session]")
{
    ApplicationGuard application;
    auto context = makeSessionContext(QStringLiteral("mailbox-session-generation-test"));
    PendingMaterializationPort materialization;
    FakeMailEvents events;
    javelin::app::MailboxSession session{"account-1",
                                         "mailbox-1",
                                         QStringLiteral("Archive"),
                                         std::optional<std::string>{"archive"},
                                         {},
                                         context.queries,
                                         materialization,
                                         100,
                                         events};

    std::size_t failureCount = 0;
    QObject::connect(&session, &javelin::app::MessageListSession::refreshFailed, &session,
                     [&failureCount](const javelin::jmap::OperationError&) { ++failureCount; });

    session.refresh();
    REQUIRE(session.state().refreshInFlight);
    session.setSort({.property = javelin::jmap::query::EmailListSortProperty::Subject,
                     .direction = javelin::jmap::query::EmailListSortDirection::Ascending});
    CHECK(session.state().stale);

    materialization.complete(javelin::jmap::OperationError{
        .message = QStringLiteral("Obsolete refresh failed."),
    });
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

    CHECK(failureCount == 0);
}

TEST_CASE("mailbox session restores a loaded infinite-scroll prefix from SQLite windows",
          "[app][mailbox-session][infinite-scroll][persistence]")
{
    ApplicationGuard application;
    auto context = makeSessionContext(QStringLiteral("mailbox-session-restore-prefix-test"));
    seedInfiniteScrollData(context.connection, true);
    PendingMaterializationPort materialization;
    FakeMailEvents events;
    javelin::app::MailboxSession session{
        "account-1",
        "mailbox-1",
        QStringLiteral("Inbox"),
        std::optional<std::string>{"inbox"},
        {},
        context.queries,
        materialization,
        2,
        events,
        javelin::app::RestoredMailboxState{
            .windows = {{.offset = 0, .limit = 2}, {.offset = 2, .limit = 2}},
        }};

    session.loadCachedState();
    waitFor([&] { return session.state().items.size() == 4; });

    CHECK(session.state().cacheLoaded);
    CHECK_FALSE(session.state().stale);
    CHECK(session.state().items[3].emailId == "email-4");
    CHECK(session.windowRequests().size() == 2);
    CHECK_FALSE(materialization.lastMailboxIntent.has_value());
}

TEST_CASE("mailbox infinite scrolling appends a bounded anchored window and ignores late IPC",
          "[app][mailbox-session][infinite-scroll]")
{
    ApplicationGuard application;
    auto context = makeSessionContext(QStringLiteral("mailbox-session-infinite-scroll-test"));
    seedInfiniteScrollData(context.connection, false);
    PendingMaterializationPort materialization;
    FakeMailEvents events;
    javelin::app::MailboxSession session{
        "account-1", "mailbox-1",     QStringLiteral("Inbox"), std::optional<std::string>{"inbox"},
        {},          context.queries, materialization,         2,
        events};

    CHECK_FALSE(session.state().stale);
    session.loadCachedState();
    waitFor([&] { return session.state().cacheLoaded; });
    REQUIRE(session.state().items.size() == 2);
    CHECK(session.state().items[0].emailId == "email-1");
    CHECK(session.state().items[1].emailId == "email-2");
    REQUIRE(session.canLoadMore());

    std::size_t stateChangeCount = 0;
    QObject::connect(&session, &javelin::app::MessageListSession::stateChanged, &session,
                     [&stateChangeCount] { ++stateChangeCount; });
    REQUIRE(session.loadMore());
    REQUIRE(materialization.lastMailboxIntent.has_value());
    CHECK(materialization.lastMailboxIntent->offset == 2);
    CHECK(materialization.lastMailboxIntent->limit == 2);
    CHECK(materialization.lastMailboxIntent->anchor == std::optional<std::string>{"email-2"});
    CHECK(materialization.lastMailboxIntent->anchorOffset == 1);
    CHECK(session.state().loadMoreInFlight);

    const auto loadMoreStateChange = stateChangeCount;
    events.publish(windowInvalidation(0, 2));
    waitFor([&] { return stateChangeCount > loadMoreStateChange; });
    CHECK(session.state().loadMoreInFlight);
    CHECK(session.state().loadMoreError.isEmpty());

    javelin::jmap::cache::MailboxWindowRepository windows{context.connection};
    REQUIRE_FALSE(windows
                      .replace({
                          .accountId = "account-1",
                          .mailboxId = "mailbox-1",
                          .queryKey = mailboxQueryKey(),
                          .requestedOffset = 2,
                          .requestedLimit = 2,
                          .position = 2,
                          .returnedLimit = 2,
                          .total = 4,
                          .queryState = "state-2",
                          .emailIds = {"email-3", "email-4"},
                      })
                      .has_value());
    events.publish(windowInvalidation(2, 2));

    waitFor([&] { return session.state().items.size() == 4; });
    CHECK_FALSE(session.state().loadMoreInFlight);
    CHECK(session.state().stale);
    CHECK(session.state().items[2].emailId == "email-3");
    CHECK(session.state().items[3].emailId == "email-4");
    CHECK_FALSE(session.canLoadMore());

    materialization.complete(javelin::jmap::OperationError{
        .message = QStringLiteral("Late continuation completion must be ignored."),
    });
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    CHECK(session.state().loadMoreError.isEmpty());
    CHECK(session.state().items.size() == 4);
}
