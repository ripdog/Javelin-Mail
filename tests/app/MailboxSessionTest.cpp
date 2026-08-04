#include "app/MailboxSession.h"
#include "app/MailApplicationEventsPorts.h"
#include "app/MessageListMaterializationPort.h"
#include "jmap/cache/Database.h"
#include "jmap/cache/QueryService.h"

#include <QCoroFuture>

#include <QCoreApplication>
#include <QEventLoop>
#include <QPromise>
#include <QTemporaryDir>

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
        requestSearchWindow(javelin::app::SearchWindowIntent) override
        {
            co_return javelin::jmap::OperationError{
                .message = QStringLiteral("Search materialization is not used by this test."),
            };
        }

        void retireSearchWindow(std::string, std::string) override
        {
        }

        void complete(javelin::app::MailboxWindowResult result)
        {
            REQUIRE_FALSE(m_completed);
            m_completed = true;
            m_mailboxPromise.addResult(std::move(result));
            m_mailboxPromise.finish();
        }

        std::optional<javelin::app::MailboxWindowIntent> lastMailboxIntent;

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

    void drainEvents()
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
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

    session.refresh();
    REQUIRE(session.page().refreshInFlight);
    REQUIRE(materialization.lastMailboxIntent.has_value());

    events.publish({
        .epoch = 1,
        .changedDomains = {javelin::protocol::ChangedDomain::MailQueryWindows},
        .affectedKeys = {QStringLiteral("mailbox-1")},
        .change =
            {
                .accountId = QStringLiteral("account-1"),
                .mailboxIds = {},
                .queryWindows = {{.mailboxId = QStringLiteral("mailbox-1"),
                                  .offset = 0,
                                  .limit = 100,
                                  .total = 0}},
                .searchWindows = {},
                .mailboxTreeChanged = false,
                .hasNewMail = false,
                .optimisticProjection = false,
                .contactsChanged = false,
            },
    });

    CHECK_FALSE(session.page().refreshInFlight);

    materialization.complete(javelin::jmap::OperationError{
        .message = QStringLiteral("Late terminal event must be ignored."),
    });
    drainEvents();

    CHECK_FALSE(session.page().refreshInFlight);
    CHECK(failureCount == 0);
}

TEST_CASE("obsolete mailbox completion cannot alter a new page", "[app][mailbox-session]")
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
                                         events,
                                         javelin::app::RestoredMailboxState{
                                             .page =
                                                 {
                                                     .offset = 0,
                                                     .installedOffset = 0,
                                                     .pendingOffset = std::nullopt,
                                                     .position = 0,
                                                     .returnedLimit = 100,
                                                     .total = 300,
                                                     .queryState = "query-state",
                                                     .anchor = std::nullopt,
                                                     .items = {},
                                                     .cacheLoaded = true,
                                                     .refreshInFlight = false,
                                                     .stale = false,
                                                     .refreshError = {},
                                                 },
                                         }};

    std::size_t failureCount = 0;
    QObject::connect(&session, &javelin::app::MessageListSession::refreshFailed, &session,
                     [&failureCount](const javelin::jmap::OperationError&) { ++failureCount; });

    session.refresh();
    REQUIRE(session.page().refreshInFlight);
    REQUIRE(session.goToPage(1));
    CHECK_FALSE(session.page().refreshInFlight);
    CHECK(session.page().offset == 100);

    materialization.complete(javelin::jmap::OperationError{
        .message = QStringLiteral("Obsolete page failed."),
    });
    drainEvents();

    CHECK_FALSE(session.page().refreshInFlight);
    CHECK(session.page().offset == 100);
    CHECK(failureCount == 0);
}
