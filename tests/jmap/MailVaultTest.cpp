#include "jmap/cache/MailVault.h"
#include "jmap/cache/Database.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/RawMessageSourceRepository.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <memory>

namespace
{
    struct DatabaseContext
    {
        QTemporaryDir directory;
        javelin::jmap::cache::DatabaseConnection connection;
    };

    [[nodiscard]] DatabaseContext database()
    {
        static int counter = 0;
        QTemporaryDir directory;
        REQUIRE(directory.isValid());
        auto result = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = QStringLiteral("mail-vault-test-%1").arg(++counter),
            .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
        });
        REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(result));
        return {.directory = std::move(directory),
                .connection =
                    std::get<javelin::jmap::cache::DatabaseConnection>(std::move(result))};
    }

    void seedAccount(javelin::jmap::cache::DatabaseConnection& connection)
    {
        QSqlQuery query{connection.database()};
        query.prepare(
            QStringLiteral("INSERT INTO accounts(account_id,email_address,session_url,is_primary) "
                           "VALUES('account-1','alice@example.com','https://example.com/jmap',1)"));
        REQUIRE(query.exec());
    }

    [[nodiscard]] javelin::jmap::domain::Email email(std::string id,
                                                     std::vector<std::string> mailboxIds)
    {
        return {.id = std::move(id),
                .blobId = "blob-1",
                .threadId = "thread-1",
                .mailboxIds = std::move(mailboxIds),
                .keywords = {},
                .size = 72,
                .receivedAt = "2026-07-21T01:00:00Z",
                .sentAt = std::nullopt,
                .messageId = {},
                .inReplyTo = {},
                .references = {},
                .hasAttachment = false,
                .subject = "Vault test",
                .from = {},
                .to = {},
                .cc = {},
                .bcc = {},
                .replyTo = {},
                .preview = "Body"};
    }
} // namespace

TEST_CASE("mail vault stores one immutable object and projects effective mailbox membership",
          "[jmap][cache][vault]")
{
    if (QCoreApplication::instance() == nullptr)
    {
        static int argc = 1;
        static char name[] = "vault-test";
        static char* argv[]{name, nullptr};
        static const auto application = std::make_unique<QCoreApplication>(argc, argv);
        Q_UNUSED(application);
    }
    auto context = database();
    seedAccount(context.connection);
    auto first = email("email-1", {"inbox"});
    auto second = email("email-2", {"archive"});
    javelin::jmap::cache::EmailRepository emails{context.connection};
    REQUIRE_FALSE(emails.upsertMany("account-1", {first, second}).has_value());

    const QByteArray payload =
        QByteArrayLiteral("From: sender@example.com\r\nSubject: Vault test\r\nContent-Type: "
                          "text/plain\r\n\r\nBody\r\n");
    javelin::jmap::cache::RawMessageSourceRepository sources{context.connection};
    REQUIRE_FALSE(
        sources
            .upsert("account-1", {.emailId = first.id, .blobId = first.blobId, .payload = payload})
            .has_value());
    REQUIRE_FALSE(sources
                      .upsert("account-1",
                              {.emailId = second.id, .blobId = second.blobId, .payload = payload})
                      .has_value());

    QSqlQuery objectCount{context.connection.database()};
    REQUIRE(objectCount.exec(QStringLiteral("SELECT COUNT(*) FROM mail_vault_objects")));
    REQUIRE(objectCount.next());
    CHECK(objectCount.value(0).toInt() == 1);
    QSqlQuery legacyCount{context.connection.database()};
    REQUIRE(legacyCount.exec(QStringLiteral("SELECT COUNT(*) FROM raw_message_sources")));
    REQUIRE(legacyCount.next());
    CHECK(legacyCount.value(0).toInt() == 0);

    const auto loaded = sources.find("account-1", "email-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::RawMessageSource>>(loaded));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::RawMessageSource>>(loaded).has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::RawMessageSource>>(loaded)->payload ==
          payload);

    const auto vault = javelin::jmap::cache::MailVault::forDatabase(context.connection);
    const auto inboxPath =
        QDir(vault.rootPath())
            .filePath(QStringLiteral("accounts/account-1/mailboxes/inbox/messages/email-1.eml"));
    const auto archivePath =
        QDir(vault.rootPath())
            .filePath(QStringLiteral("accounts/account-1/mailboxes/archive/messages/email-2.eml"));
    REQUIRE(QFileInfo::exists(inboxPath));
    REQUIRE(QFileInfo::exists(archivePath));
    CHECK(std::filesystem::equivalent(inboxPath.toStdString(), archivePath.toStdString()));

    first.mailboxIds = {"archive"};
    REQUIRE_FALSE(emails.upsertMany("account-1", {first}).has_value());
    REQUIRE_FALSE(sources.replayProjectionJobs().has_value());
    CHECK_FALSE(QFileInfo::exists(inboxPath));
    const auto movedPath =
        QDir(vault.rootPath())
            .filePath(QStringLiteral("accounts/account-1/mailboxes/archive/messages/email-1.eml"));
    REQUIRE(QFileInfo::exists(movedPath));

    const std::array removedIds{first.id};
    REQUIRE_FALSE(emails.removeFromMailbox("account-1", "archive", removedIds).has_value());
    REQUIRE_FALSE(sources.replayProjectionJobs().has_value());
    CHECK_FALSE(QFileInfo::exists(movedPath));
}

TEST_CASE("mail vault leases prevent eviction and release on disconnect",
          "[jmap][cache][vault][lease]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const javelin::jmap::cache::MailVault vault{directory.path()};
    const auto installed = vault.install(QByteArrayLiteral("leased payload"));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::MailVaultObject>(installed));
    const auto object = std::get<javelin::jmap::cache::MailVaultObject>(installed);

    auto leaseResult = vault.acquireLease(object);
    REQUIRE(std::holds_alternative<javelin::jmap::cache::MailVaultLease>(leaseResult));
    auto lease = std::get<javelin::jmap::cache::MailVaultLease>(std::move(leaseResult));
    CHECK(lease.isValid());
    CHECK(std::get<QByteArray>(lease.read()) == QByteArrayLiteral("leased payload"));
    CHECK(vault.evict(object).has_value());

    lease = {};
    CHECK_FALSE(vault.evict(object).has_value());

    const auto staged = vault.stage(QByteArrayLiteral("staged payload"));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::MailVaultObject>(staged));
    auto stagedLeaseResult =
        vault.acquireLease(std::get<javelin::jmap::cache::MailVaultObject>(staged));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::MailVaultLease>(stagedLeaseResult));
    auto stagedLease = std::get<javelin::jmap::cache::MailVaultLease>(std::move(stagedLeaseResult));
    javelin::jmap::cache::MailVault::releaseAllLeases();
    CHECK_FALSE(stagedLease.isValid());
    CHECK(vault.evict(std::get<javelin::jmap::cache::MailVaultObject>(staged)).has_value() ==
          false);
}

TEST_CASE("mail vault eviction removes projections and respects active leases",
          "[jmap][cache][vault][eviction]")
{
    if (QCoreApplication::instance() == nullptr)
    {
        static int argc = 1;
        static char name[] = "vault-eviction-test";
        static char* argv[]{name, nullptr};
        static const auto application = std::make_unique<QCoreApplication>(argc, argv);
        Q_UNUSED(application);
    }
    auto context = database();
    seedAccount(context.connection);
    const auto message = email("email-evict", {"inbox"});
    javelin::jmap::cache::EmailRepository emails{context.connection};
    REQUIRE_FALSE(emails.upsertMany("account-1", {message}).has_value());

    const QByteArray payload = QByteArrayLiteral("evictable payload");
    javelin::jmap::cache::RawMessageSourceRepository sources{context.connection};
    REQUIRE_FALSE(sources
                      .upsert("account-1",
                              {.emailId = message.id, .blobId = message.blobId, .payload = payload})
                      .has_value());

    QSqlQuery markIndexed{context.connection.database()};
    REQUIRE(markIndexed.exec(QStringLiteral(
        "UPDATE mail_vault_email_refs SET indexed_hash=content_hash WHERE account_id="
        "'account-1' AND email_id='email-evict'")));

    QSqlQuery objectQuery{context.connection.database()};
    REQUIRE(objectQuery.exec(QStringLiteral(
        "SELECT o.content_hash,o.relative_path,o.size FROM mail_vault_objects o JOIN "
        "mail_vault_email_refs r ON r.content_hash=o.content_hash WHERE r.account_id="
        "'account-1' AND r.email_id='email-evict'")));
    REQUIRE(objectQuery.next());
    const javelin::jmap::cache::MailVaultObject object{
        .contentHash = objectQuery.value(0).toString().toStdString(),
        .relativePath = objectQuery.value(1).toString(),
        .size = objectQuery.value(2).toULongLong(),
    };
    const auto vault = javelin::jmap::cache::MailVault::forDatabase(context.connection);
    const auto projectionPath =
        QDir(vault.rootPath())
            .filePath(QStringLiteral("accounts/account-1/mailboxes/inbox/messages/"
                                     "email-evict.eml"));
    REQUIRE(QFileInfo::exists(projectionPath));

    auto leaseResult = vault.acquireLease(object);
    REQUIRE(std::holds_alternative<javelin::jmap::cache::MailVaultLease>(leaseResult));
    auto lease = std::get<javelin::jmap::cache::MailVaultLease>(std::move(leaseResult));
    const auto blocked = sources.evictUnretained(10);
    REQUIRE(std::holds_alternative<std::size_t>(blocked));
    CHECK(std::get<std::size_t>(blocked) == 0);
    CHECK(QFileInfo::exists(projectionPath));

    lease = {};
    const auto evicted = sources.evictUnretained(10);
    REQUIRE(std::holds_alternative<std::size_t>(evicted));
    CHECK(std::get<std::size_t>(evicted) == 1);
    CHECK_FALSE(QFileInfo::exists(projectionPath));

    QSqlQuery rows{context.connection.database()};
    REQUIRE(rows.exec(QStringLiteral(
        "SELECT (SELECT COUNT(*) FROM mail_vault_objects), (SELECT COUNT(*) FROM "
        "mail_vault_email_refs), (SELECT COUNT(*) FROM mail_vault_projection_jobs)")));
    REQUIRE(rows.next());
    CHECK(rows.value(0).toInt() == 0);
    CHECK(rows.value(1).toInt() == 0);
    CHECK(rows.value(2).toInt() == 0);
}
