#include "app/SearchSession.h"
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

        void complete(javelin::app::SearchWindowResult result)
        {
            REQUIRE_FALSE(m_completed);
            m_completed = true;
            m_searchPromise.addResult(std::move(result));
            m_searchPromise.finish();
        }

        std::optional<javelin::app::SearchWindowIntent> lastSearchIntent;

      private:
        QPromise<javelin::app::SearchWindowResult> m_searchPromise;
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

    [[nodiscard]] SessionContext makeSessionContext()
    {
        QTemporaryDir directory;
        REQUIRE(directory.isValid());
        auto opened = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = QStringLiteral("search-session-commit-test"),
            .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
        });
        REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
        return SessionContext{
            std::move(directory),
            std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened)),
        };
    }
} // namespace

TEST_CASE("search cache commit terminates its visible refresh", "[app][search-session]")
{
    ApplicationGuard application;
    auto context = makeSessionContext();
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
    REQUIRE(session.page().refreshInFlight);
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
                .hasNewMail = false,
                .optimisticProjection = false,
                .contactsChanged = false,
            },
    });

    CHECK_FALSE(session.page().refreshInFlight);

    materialization.complete(javelin::jmap::OperationError{
        .message = QStringLiteral("Late search terminal event must be ignored."),
    });
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

    CHECK_FALSE(session.page().refreshInFlight);
    CHECK(failureCount == 0);
}
