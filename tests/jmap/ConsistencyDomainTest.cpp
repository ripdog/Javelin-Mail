#include "jmap/sync/ConsistencyDomain.h"
#include "jmap/sync/MutationJournal.h"

#include <QCoreApplication>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <variant>

namespace
{

    class ApplicationGuard
    {
      public:
        ApplicationGuard()
        {
            if (QCoreApplication::instance() != nullptr)
            {
                return;
            }

            static int argc = 1;
            static char appName[] = "javelin-tests";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    [[nodiscard]] javelin::jmap::cache::DatabaseConnection makeConnection(QTemporaryDir& directory)
    {
        static int counter = 0;
        auto result = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = QStringLiteral("javelin-consistency-domain-%1").arg(++counter),
            .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
        });
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
        {
            FAIL(error->message.toStdString());
        }
        return std::get<javelin::jmap::cache::DatabaseConnection>(std::move(result));
    }

    void seedAccount(javelin::jmap::cache::DatabaseConnection& connection)
    {
        QSqlQuery query{connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO accounts (account_id,email_address,session_url,is_primary) "
            "VALUES ('account-1','alice@example.com','https://example.com/jmap',1)"));
        REQUIRE(query.exec());
    }

} // namespace

TEST_CASE("consistency domains fence refreshes independently by JMAP data type",
          "[jmap][sync][consistency]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto connection = makeConnection(directory);
    seedAccount(connection);

    javelin::jmap::sync::ConsistencyDomainRepository repository{connection};
    const javelin::jmap::sync::ConsistencyDomain emailDomain{
        .accountId = "account-1",
        .dataType = "Email",
    };
    const javelin::jmap::sync::ConsistencyDomain contactDomain{
        .accountId = "account-1",
        .dataType = "ContactCard",
    };

    const auto emailFenceResult = repository.captureRefresh(emailDomain);
    REQUIRE(std::holds_alternative<javelin::jmap::sync::RefreshFence>(emailFenceResult));
    const auto emailFence = std::get<javelin::jmap::sync::RefreshFence>(emailFenceResult);

    const auto contactFenceResult = repository.captureRefresh(contactDomain);
    REQUIRE(std::holds_alternative<javelin::jmap::sync::RefreshFence>(contactFenceResult));
    const auto contactFence = std::get<javelin::jmap::sync::RefreshFence>(contactFenceResult);

    const auto advanced = repository.advanceMutation(emailDomain);
    REQUIRE(std::holds_alternative<std::uint64_t>(advanced));
    CHECK(std::get<std::uint64_t>(advanced) == 1);

    const auto emailCurrent = repository.isCurrent(emailFence);
    REQUIRE(std::holds_alternative<bool>(emailCurrent));
    CHECK_FALSE(std::get<bool>(emailCurrent));

    const auto contactCurrent = repository.isCurrent(contactFence);
    REQUIRE(std::holds_alternative<bool>(contactCurrent));
    CHECK(std::get<bool>(contactCurrent));
}

TEST_CASE("consistency-domain generations persist and advance monotonically",
          "[jmap][sync][consistency]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto connection = makeConnection(directory);
    seedAccount(connection);

    javelin::jmap::sync::ConsistencyDomainRepository repository{connection};
    const javelin::jmap::sync::ConsistencyDomain domain{
        .accountId = "account-1",
        .dataType = "CalendarEvent",
    };

    const auto first = repository.advanceMutation(domain);
    const auto second = repository.advanceMutation(domain);
    REQUIRE(std::holds_alternative<std::uint64_t>(first));
    REQUIRE(std::holds_alternative<std::uint64_t>(second));
    CHECK(std::get<std::uint64_t>(first) == 1);
    CHECK(std::get<std::uint64_t>(second) == 2);

    javelin::jmap::sync::ConsistencyDomainRepository reopenedView{connection};
    const auto generation = reopenedView.mutationGeneration(domain);
    REQUIRE(std::holds_alternative<std::uint64_t>(generation));
    CHECK(std::get<std::uint64_t>(generation) == 2);
}

TEST_CASE("active optimistic mutations prevent refresh commits without changing causality",
          "[jmap][sync][consistency]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto connection = makeConnection(directory);
    seedAccount(connection);

    const javelin::jmap::sync::ConsistencyDomain domain{
        .accountId = "account-1",
        .dataType = "ContactCard",
    };
    javelin::jmap::sync::ConsistencyDomainRepository consistency{connection};
    const auto fenceResult = consistency.captureRefresh(domain);
    REQUIRE(std::holds_alternative<javelin::jmap::sync::RefreshFence>(fenceResult));
    const auto fence = std::get<javelin::jmap::sync::RefreshFence>(fenceResult);

    javelin::jmap::sync::MutationJournalRepository journal{connection};
    REQUIRE_FALSE(journal
                      .put({
                          .mutationId = "contact-1",
                          .operationGroupId = std::nullopt,
                          .domain = domain,
                          .objectId = "card-1",
                          .mutationKind = "contact_card_patch",
                          .status = javelin::jmap::sync::MutationStatus::Pending,
                          .payloadJson = "{}",
                          .baseState = "c1",
                          .acceptedState = std::nullopt,
                          .errorJson = std::nullopt,
                      })
                      .has_value());

    const auto current = consistency.isCurrent(fence);
    REQUIRE(std::holds_alternative<bool>(current));
    CHECK(std::get<bool>(current));
    const auto blocked = consistency.canCommitRefresh(fence);
    REQUIRE(std::holds_alternative<bool>(blocked));
    CHECK_FALSE(std::get<bool>(blocked));

    REQUIRE_FALSE(
        journal.transition("contact-1", javelin::jmap::sync::MutationStatus::Rejected).has_value());
    const auto unblocked = consistency.canCommitRefresh(fence);
    REQUIRE(std::holds_alternative<bool>(unblocked));
    CHECK(std::get<bool>(unblocked));
}
