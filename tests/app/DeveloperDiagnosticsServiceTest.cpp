#include "app/DeveloperDiagnosticsService.h"

#include "jmap/cache/MailVault.h"
#include "jmap/cache/RawMessageSourceRepository.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <ranges>
#include <string_view>

namespace
{
    void ensureApplication()
    {
        if (QCoreApplication::instance() != nullptr)
            return;
        static int argc = 1;
        static char name[] = "developer-diagnostics-test";
        static char* argv[]{name, nullptr};
        static const auto application = std::make_unique<QCoreApplication>(argc, argv);
        Q_UNUSED(application);
    }

    void execute(QSqlDatabase& database, const QString& statement)
    {
        QSqlQuery query{database};
        INFO(statement.toStdString());
        REQUIRE(query.exec(statement));
    }

    [[nodiscard]] const javelin::app::DeveloperMailboxRecord&
    mailbox(const javelin::app::DeveloperDiagnosticsSnapshot& snapshot,
            const std::string_view mailboxId)
    {
        const auto found = std::ranges::find_if(
            snapshot.mailboxes, [mailboxId](const javelin::app::DeveloperMailboxRecord& candidate)
            { return candidate.mailboxId.toStdString() == mailboxId; });
        REQUIRE(found != snapshot.mailboxes.end());
        return *found;
    }
} // namespace

TEST_CASE("developer diagnostics measures shared and reclaimable mailbox bodies",
          "[app][developer-diagnostics]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("cache.sqlite3"));
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("developer-diagnostics-service-test"),
        .databasePath = databasePath,
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));

    execute(
        connection.database(),
        QStringLiteral("INSERT INTO accounts(account_id,email_address,session_url,is_primary) "
                       "VALUES('account-1','person@example.test','https://example.test/jmap',1)"));
    execute(connection.database(),
            QStringLiteral("INSERT INTO mailboxes(account_id,mailbox_id,name,role,is_subscribed) "
                           "VALUES('account-1','inbox','Inbox','inbox',1),"
                           "('account-1','archive','Archive','archive',1)"));
    execute(
        connection.database(),
        QStringLiteral("INSERT INTO emails(account_id,email_id,thread_id,blob_id,subject,size) "
                       "VALUES('account-1','shared-inbox','thread-1','blob-shared','Shared',13),"
                       "('account-1','shared-archive','thread-2','blob-shared','Shared',13),"
                       "('account-1','unique-inbox','thread-3','blob-unique','Unique',17)"));
    execute(connection.database(),
            QStringLiteral("INSERT INTO email_mailboxes(account_id,email_id,mailbox_id) VALUES"
                           "('account-1','shared-inbox','inbox'),"
                           "('account-1','shared-archive','archive'),"
                           "('account-1','unique-inbox','inbox')"));

    javelin::jmap::cache::RawMessageSourceRepository sources{connection};
    const QByteArray sharedPayload = QByteArrayLiteral("shared body\r\n");
    const QByteArray uniquePayload = QByteArrayLiteral("unique inbox body\r\n");
    REQUIRE_FALSE(
        sources
            .upsert("account-1",
                    {.emailId = "shared-inbox", .blobId = "blob-shared", .payload = sharedPayload})
            .has_value());
    REQUIRE_FALSE(sources
                      .upsert("account-1", {.emailId = "shared-archive",
                                            .blobId = "blob-shared",
                                            .payload = sharedPayload})
                      .has_value());
    REQUIRE_FALSE(
        sources
            .upsert("account-1",
                    {.emailId = "unique-inbox", .blobId = "blob-unique", .payload = uniquePayload})
            .has_value());

    const auto vault = javelin::jmap::cache::MailVault::forDatabase(connection);
    javelin::app::DeveloperDiagnosticsService diagnostics{databasePath, vault.rootPath()};
    const auto result = QCoro::waitFor(diagnostics.snapshot());
    const auto* snapshot = std::get_if<javelin::app::DeveloperDiagnosticsSnapshot>(&result);
    REQUIRE(snapshot != nullptr);
    REQUIRE(snapshot->mailboxes.size() == 2);

    const auto& inbox = mailbox(*snapshot, "inbox");
    CHECK(inbox.accountEmailAddress == QStringLiteral("person@example.test"));
    CHECK(inbox.role == QStringLiteral("inbox"));
    CHECK(inbox.cachedMembershipCount == 2);
    CHECK(inbox.usage.logicalBodyBytes ==
          static_cast<std::uint64_t>(sharedPayload.size() + uniquePayload.size()));
    CHECK(inbox.usage.sharedBodyBytes == static_cast<std::uint64_t>(sharedPayload.size()));
    CHECK(inbox.usage.reclaimableBodyBytes == static_cast<std::uint64_t>(uniquePayload.size()));
    CHECK(inbox.usage.allocatedBodyBytes >= inbox.usage.logicalBodyBytes);
    CHECK(inbox.usage.missingBodyObjects == 0);

    const auto& archive = mailbox(*snapshot, "archive");
    CHECK(archive.cachedMembershipCount == 1);
    CHECK(archive.usage.logicalBodyBytes == static_cast<std::uint64_t>(sharedPayload.size()));
    CHECK(archive.usage.sharedBodyBytes == static_cast<std::uint64_t>(sharedPayload.size()));
    CHECK(archive.usage.reclaimableBodyBytes == 0);
}
