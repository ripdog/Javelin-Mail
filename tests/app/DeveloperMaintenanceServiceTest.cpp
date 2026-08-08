#include "app/DeveloperMaintenanceService.h"

#include "app/MailboxMaintenanceRegistry.h"
#include "app/WorkScheduler.h"
#include "jmap/cache/Database.h"
#include "jmap/cache/MailVault.h"
#include "jmap/cache/RawMessageSourceRepository.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using javelin::app::DeveloperMailboxCacheKind;
    using javelin::app::DeveloperMailboxClearCommand;
    using javelin::app::DeveloperMailboxClearResult;
    using javelin::app::DeveloperMaintenanceService;
    using javelin::app::DeveloperOfflineClearPolicy;
    using javelin::app::WorkRecord;
    using javelin::app::WorkScheduler;
    using javelin::app::WorkStatus;
    using javelin::jmap::cache::DatabaseConnection;

    struct DatabaseContext
    {
        QTemporaryDir directory;
        DatabaseConnection connection;
    };

    class RecordingPublisher final : public javelin::app::MailCacheChangePublisher
    {
      public:
        void publishCacheChange(javelin::app::MailCacheChange change) override
        {
            changes.push_back(std::move(change));
        }

        std::vector<javelin::app::MailCacheChange> changes;
    };

    void ensureApplication()
    {
        if (QCoreApplication::instance() != nullptr)
            return;
        static int argc = 1;
        static char name[] = "developer-maintenance-test";
        static char* argv[]{name, nullptr};
        static const auto application = std::make_unique<QCoreApplication>(argc, argv);
        Q_UNUSED(application);
    }

    [[nodiscard]] DatabaseContext database()
    {
        static int counter = 0;
        QTemporaryDir directory;
        REQUIRE(directory.isValid());
        auto opened = DatabaseConnection::open({
            .connectionName = QStringLiteral("developer-maintenance-test-%1").arg(++counter),
            .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
        });
        REQUIRE(std::holds_alternative<DatabaseConnection>(opened));
        return {.directory = std::move(directory),
                .connection = std::get<DatabaseConnection>(std::move(opened))};
    }

    void execute(QSqlDatabase& database, const QString& statement)
    {
        QSqlQuery query{database};
        INFO(statement.toStdString());
        REQUIRE(query.exec(statement));
    }

    [[nodiscard]] std::uint64_t scalar(QSqlDatabase& database, const QString& statement)
    {
        QSqlQuery query{database};
        INFO(statement.toStdString());
        REQUIRE(query.exec(statement));
        REQUIRE(query.next());
        return query.value(0).toULongLong();
    }

    [[nodiscard]] QString textScalar(QSqlDatabase& database, const QString& statement)
    {
        QSqlQuery query{database};
        INFO(statement.toStdString());
        REQUIRE(query.exec(statement));
        REQUIRE(query.next());
        return query.value(0).toString();
    }

    void seedBase(DatabaseConnection& connection)
    {
        auto& database = connection.database();
        execute(database,
                QStringLiteral("INSERT INTO accounts(account_id,email_address,session_url,"
                               "is_primary) VALUES('account-1','person@example.test',"
                               "'https://example.test/jmap',1)"));
        execute(database,
                QStringLiteral("INSERT INTO mailboxes(account_id,mailbox_id,name,role,"
                               "is_subscribed) VALUES('account-1','inbox','Inbox','inbox',1),"
                               "('account-1','archive','Archive','archive',1)"));
        execute(database, QStringLiteral("INSERT INTO consistency_domains(account_id,data_type,"
                                         "mutation_generation) VALUES('account-1','Email',4)"));
    }

    void seedEmail(QSqlDatabase& database, const QString& emailId, const QString& blobId,
                   const QString& mailboxId)
    {
        execute(database,
                QStringLiteral("INSERT INTO emails(account_id,email_id,thread_id,blob_id,subject,"
                               "size) VALUES('account-1','%1','thread-%1','%2','%1',10)")
                    .arg(emailId, blobId));
        execute(database,
                QStringLiteral("INSERT INTO email_mailboxes(account_id,email_id,mailbox_id) "
                               "VALUES('account-1','%1','%2')")
                    .arg(emailId, mailboxId));
    }

    [[nodiscard]] QString queuedJobId(const DeveloperMailboxClearResult& result)
    {
        const auto* value = std::get_if<javelin::app::DeveloperMailboxClearQueued>(&result);
        REQUIRE(value != nullptr);
        REQUIRE_FALSE(value->jobId.isEmpty());
        return value->jobId;
    }

    [[nodiscard]] WorkRecord waitForTerminalJob(WorkScheduler& scheduler, const QString& jobId)
    {
        QElapsedTimer timeout;
        timeout.start();
        while (timeout.elapsed() < 5000)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            const auto found = scheduler.find(jobId.toStdString());
            const auto* record = std::get_if<std::optional<WorkRecord>>(&found);
            if (record != nullptr && record->has_value() &&
                ((*record)->status == WorkStatus::Complete ||
                 (*record)->status == WorkStatus::Failed))
                return **record;
            QThread::msleep(1);
        }
        FAIL("background mailbox cache cleanup did not finish");
        return {};
    }
} // namespace

TEST_CASE("mailbox maintenance registry serializes the same target",
          "[app][developer-maintenance][registry]")
{
    javelin::app::MailboxMaintenanceRegistry registry;
    auto first = registry.tryBegin(QStringLiteral("account-1"), QStringLiteral("inbox"));
    REQUIRE(first.has_value());
    CHECK(first->generation() == 1);
    CHECK(registry.isActive(QStringLiteral("account-1"), QStringLiteral("inbox")));
    CHECK_FALSE(
        registry.tryBegin(QStringLiteral("account-1"), QStringLiteral("inbox")).has_value());

    auto other = registry.tryBegin(QStringLiteral("account-1"), QStringLiteral("archive"));
    REQUIRE(other.has_value());
    CHECK(other->generation() == 1);

    first.reset();
    CHECK_FALSE(registry.isActive(QStringLiteral("account-1"), QStringLiteral("inbox")));
    auto second = registry.tryBegin(QStringLiteral("account-1"), QStringLiteral("inbox"));
    REQUIRE(second.has_value());
    CHECK(second->generation() == 2);
}

TEST_CASE("developer SQLite clear preserves mailbox bodies and active optimistic membership",
          "[app][developer-maintenance][sqlite]")
{
    ensureApplication();
    auto context = database();
    seedBase(context.connection);
    auto& database = context.connection.database();
    seedEmail(database, QStringLiteral("ordinary"), QStringLiteral("blob-ordinary"),
              QStringLiteral("inbox"));
    seedEmail(database, QStringLiteral("optimistic"), QStringLiteral("blob-optimistic"),
              QStringLiteral("inbox"));
    seedEmail(database, QStringLiteral("archive-email"), QStringLiteral("blob-archive"),
              QStringLiteral("archive"));

    execute(database,
            QStringLiteral("INSERT INTO mailbox_query_windows(account_id,mailbox_id,query_key,"
                           "requested_offset,requested_limit,position,returned_limit,total,"
                           "query_state) VALUES('account-1','inbox','inbox-query',0,100,0,100,2,"
                           "'q1'),('account-1','archive','archive-query',0,100,0,100,1,'q2')"));
    execute(database, QStringLiteral("INSERT INTO mailbox_query_window_items(account_id,query_key,"
                                     "requested_offset,requested_limit,position,email_id) VALUES"
                                     "('account-1','inbox-query',0,100,0,'ordinary'),"
                                     "('account-1','inbox-query',0,100,1,'optimistic'),"
                                     "('account-1','archive-query',0,100,0,'archive-email')"));
    execute(database,
            QStringLiteral("INSERT INTO offline_mailbox_scopes(account_id,mailbox_id,desired,"
                           "status,generation,completed_generation,completed_total) VALUES"
                           "('account-1','inbox',1,'complete',3,3,2),"
                           "('account-1','archive',1,'complete',8,8,1)"));
    execute(database,
            QStringLiteral("INSERT INTO offline_mailbox_membership(account_id,mailbox_id,email_id,"
                           "generation,position) VALUES('account-1','inbox','ordinary',3,0),"
                           "('account-1','inbox','optimistic',3,1),"
                           "('account-1','archive','archive-email',8,0)"));
    execute(database,
            QStringLiteral("INSERT INTO mutation_journal(mutation_id,account_id,data_type,"
                           "object_id,mutation_kind,status,payload_json,sequence) VALUES"
                           "('mutation-1','account-1','Email','optimistic','email_patch','pending',"
                           "'{}',1)"));

    javelin::jmap::cache::RawMessageSourceRepository sources{context.connection};
    REQUIRE_FALSE(sources
                      .upsert("account-1", {.emailId = "ordinary",
                                            .blobId = "blob-ordinary",
                                            .payload = QByteArrayLiteral("ordinary body")})
                      .has_value());
    REQUIRE_FALSE(sources
                      .upsert("account-1", {.emailId = "optimistic",
                                            .blobId = "blob-optimistic",
                                            .payload = QByteArrayLiteral("optimistic body")})
                      .has_value());

    RecordingPublisher publisher;
    javelin::app::MailboxMaintenanceRegistry registry;
    WorkScheduler scheduler{context.connection, nullptr, std::chrono::milliseconds{0}};
    std::optional<std::pair<std::string, std::string>> resync;
    DeveloperMaintenanceService maintenance{
        context.directory.filePath(QStringLiteral("cache.sqlite3")),
        javelin::jmap::cache::MailVault::forDatabase(context.connection).rootPath(),
        registry,
        publisher,
        scheduler,
        [&resync](const std::string_view accountId, const std::string_view mailboxId)
        { resync = std::pair{std::string{accountId}, std::string{mailboxId}}; }};

    const auto result = QCoro::waitFor(maintenance.clearMailboxCache({
        .accountId = QStringLiteral("account-1"),
        .mailboxId = QStringLiteral("inbox"),
        .kind = DeveloperMailboxCacheKind::Sqlite,
        .offlinePolicy = DeveloperOfflineClearPolicy::Preserve,
    }));
    const auto completed = waitForTerminalJob(scheduler, queuedJobId(result));
    REQUIRE(completed.status == WorkStatus::Complete);

    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM mailboxes WHERE account_id="
                                          "'account-1' AND mailbox_id='inbox'")) == 1);
    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM email_mailboxes WHERE account_id="
                                          "'account-1' AND mailbox_id='inbox'")) == 1);
    CHECK(
        textScalar(database, QStringLiteral("SELECT email_id FROM email_mailboxes WHERE account_id="
                                            "'account-1' AND mailbox_id='inbox'")) ==
        QStringLiteral("optimistic"));
    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM email_mailboxes WHERE account_id="
                                          "'account-1' AND mailbox_id='archive'")) == 1);
    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM mailbox_query_windows WHERE "
                                          "account_id='account-1' AND mailbox_id='inbox'")) == 0);
    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM mailbox_query_windows WHERE "
                                          "account_id='account-1' AND mailbox_id='archive'")) == 1);
    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM offline_mailbox_membership WHERE "
                                          "account_id='account-1' AND mailbox_id='inbox'")) == 0);
    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM offline_mailbox_membership WHERE "
                                          "account_id='account-1' AND mailbox_id='archive'")) == 1);
    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM mail_vault_mailbox_refs WHERE "
                                          "account_id='account-1' AND mailbox_id='inbox'")) == 2);
    CHECK(scalar(database, QStringLiteral("SELECT desired FROM offline_mailbox_scopes WHERE "
                                          "account_id='account-1' AND mailbox_id='inbox'")) == 1);
    CHECK(textScalar(database, QStringLiteral("SELECT status FROM offline_mailbox_scopes WHERE "
                                              "account_id='account-1' AND mailbox_id='inbox'")) ==
          QStringLiteral("pending"));
    CHECK(scalar(database, QStringLiteral("SELECT generation FROM offline_mailbox_scopes WHERE "
                                          "account_id='account-1' AND mailbox_id='inbox'")) == 4);
    CHECK(scalar(database, QStringLiteral("SELECT mutation_generation FROM consistency_domains "
                                          "WHERE account_id='account-1' AND data_type='Email'")) ==
          5);
    REQUIRE(publisher.changes.size() == 1);
    CHECK(publisher.changes.front().mailboxIds == QStringList{QStringLiteral("inbox")});
    REQUIRE(resync.has_value());
    CHECK(resync->first == "account-1");
    CHECK(resync->second == "inbox");
}

TEST_CASE("combined mailbox clear removes bodies before cached mailbox state",
          "[app][developer-maintenance][combined]")
{
    ensureApplication();
    auto context = database();
    seedBase(context.connection);
    auto& database = context.connection.database();
    seedEmail(database, QStringLiteral("message-1"), QStringLiteral("blob-1"),
              QStringLiteral("inbox"));
    execute(database,
            QStringLiteral("INSERT INTO offline_mailbox_scopes(account_id,mailbox_id,desired,"
                           "status,generation,completed_generation,completed_total) VALUES"
                           "('account-1','inbox',1,'complete',2,2,1)"));
    execute(database,
            QStringLiteral("INSERT INTO offline_mailbox_membership(account_id,mailbox_id,email_id,"
                           "generation,position) VALUES('account-1','inbox','message-1',2,0)"));

    const QByteArray payload = QByteArrayLiteral("combined clear body");
    javelin::jmap::cache::RawMessageSourceRepository sources{context.connection};
    REQUIRE_FALSE(
        sources
            .upsert("account-1", {.emailId = "message-1", .blobId = "blob-1", .payload = payload})
            .has_value());
    const auto vault = javelin::jmap::cache::MailVault::forDatabase(context.connection);
    const QString bodyPath = textScalar(
        database, QStringLiteral("SELECT relative_path FROM mail_vault_objects o JOIN "
                                 "mail_vault_email_refs r ON r.content_hash=o.content_hash WHERE "
                                 "r.account_id='account-1' AND r.email_id='message-1'"));
    REQUIRE(QFileInfo::exists(QDir{vault.rootPath()}.filePath(bodyPath)));

    RecordingPublisher publisher;
    javelin::app::MailboxMaintenanceRegistry registry;
    WorkScheduler scheduler{context.connection, nullptr, std::chrono::milliseconds{0}};
    bool resyncRequested = false;
    DeveloperMaintenanceService maintenance{
        context.directory.filePath(QStringLiteral("cache.sqlite3")),
        vault.rootPath(),
        registry,
        publisher,
        scheduler,
        [&resyncRequested](const std::string_view, const std::string_view)
        { resyncRequested = true; }};

    const auto result = QCoro::waitFor(maintenance.clearMailboxCache({
        .accountId = QStringLiteral("account-1"),
        .mailboxId = QStringLiteral("inbox"),
        .kind = DeveloperMailboxCacheKind::SqliteAndBodies,
        .offlinePolicy = DeveloperOfflineClearPolicy::Disable,
    }));
    const auto completed = waitForTerminalJob(scheduler, queuedJobId(result));
    REQUIRE(completed.status == WorkStatus::Complete);
    CHECK(completed.progress.completedBytes == static_cast<std::uint64_t>(payload.size()));

    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM mail_vault_mailbox_refs WHERE "
                                          "account_id='account-1' AND mailbox_id='inbox'")) == 0);
    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM mail_vault_email_refs WHERE "
                                          "account_id='account-1' AND email_id='message-1'")) == 0);
    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM email_mailboxes WHERE account_id="
                                          "'account-1' AND mailbox_id='inbox'")) == 0);
    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM offline_mailbox_membership WHERE "
                                          "account_id='account-1' AND mailbox_id='inbox'")) == 0);
    CHECK(scalar(database, QStringLiteral("SELECT desired FROM offline_mailbox_scopes WHERE "
                                          "account_id='account-1' AND mailbox_id='inbox'")) == 0);
    CHECK_FALSE(QFileInfo::exists(QDir{vault.rootPath()}.filePath(bodyPath)));
    CHECK_FALSE(resyncRequested);
    REQUIRE(publisher.changes.size() == 1);
}

TEST_CASE("developer body clear reclaims unique objects and preserves shared bodies",
          "[app][developer-maintenance][bodies]")
{
    ensureApplication();
    auto context = database();
    seedBase(context.connection);
    auto& database = context.connection.database();
    seedEmail(database, QStringLiteral("shared-inbox"), QStringLiteral("blob-shared"),
              QStringLiteral("inbox"));
    seedEmail(database, QStringLiteral("shared-archive"), QStringLiteral("blob-shared"),
              QStringLiteral("archive"));
    seedEmail(database, QStringLiteral("unique-inbox"), QStringLiteral("blob-unique"),
              QStringLiteral("inbox"));
    execute(database,
            QStringLiteral("INSERT INTO offline_mailbox_scopes(account_id,mailbox_id,desired,"
                           "status,generation,completed_generation,completed_total) VALUES"
                           "('account-1','inbox',1,'complete',3,3,2)"));

    const QByteArray sharedPayload = QByteArrayLiteral("shared body");
    const QByteArray uniquePayload = QByteArrayLiteral("unique body only in inbox");
    javelin::jmap::cache::RawMessageSourceRepository sources{context.connection};
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
    execute(database,
            QStringLiteral("INSERT INTO mail_vault_projection_jobs(account_id,email_id,mailbox_id,"
                           "operation) VALUES('account-1','shared-archive','archive','metadata')"));

    const QString uniquePath = textScalar(
        database, QStringLiteral("SELECT relative_path FROM mail_vault_objects o JOIN "
                                 "mail_vault_email_refs r ON r.content_hash=o.content_hash WHERE "
                                 "r.account_id='account-1' AND r.email_id='unique-inbox'"));
    const auto vault = javelin::jmap::cache::MailVault::forDatabase(context.connection);
    REQUIRE(QFileInfo::exists(QDir{vault.rootPath()}.filePath(uniquePath)));

    RecordingPublisher publisher;
    javelin::app::MailboxMaintenanceRegistry registry;
    WorkScheduler scheduler{context.connection, nullptr, std::chrono::milliseconds{0}};
    bool resyncRequested = false;
    DeveloperMaintenanceService maintenance{
        context.directory.filePath(QStringLiteral("cache.sqlite3")),
        vault.rootPath(),
        registry,
        publisher,
        scheduler,
        [&resyncRequested](const std::string_view, const std::string_view)
        { resyncRequested = true; }};

    const auto result = QCoro::waitFor(maintenance.clearMailboxCache({
        .accountId = QStringLiteral("account-1"),
        .mailboxId = QStringLiteral("inbox"),
        .kind = DeveloperMailboxCacheKind::Bodies,
        .offlinePolicy = DeveloperOfflineClearPolicy::Disable,
    }));
    const auto completed = waitForTerminalJob(scheduler, queuedJobId(result));
    REQUIRE(completed.status == WorkStatus::Complete);
    CHECK(completed.progress.completedBytes == static_cast<std::uint64_t>(uniquePayload.size()));

    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM email_mailboxes WHERE account_id="
                                          "'account-1' AND mailbox_id='inbox'")) == 2);
    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM mail_vault_mailbox_refs WHERE "
                                          "account_id='account-1' AND mailbox_id='inbox'")) == 0);
    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM mail_vault_mailbox_refs WHERE "
                                          "account_id='account-1' AND mailbox_id='archive'")) == 1);
    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM mail_vault_email_refs WHERE "
                                          "account_id='account-1' AND email_id='shared-inbox'")) ==
          0);
    CHECK(scalar(database,
                 QStringLiteral("SELECT COUNT(*) FROM mail_vault_email_refs WHERE "
                                "account_id='account-1' AND email_id='shared-archive'")) == 1);
    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM mail_vault_email_refs WHERE "
                                          "account_id='account-1' AND email_id='unique-inbox'")) ==
          0);
    CHECK_FALSE(QFileInfo::exists(QDir{vault.rootPath()}.filePath(uniquePath)));
    CHECK(scalar(database, QStringLiteral("SELECT desired FROM offline_mailbox_scopes WHERE "
                                          "account_id='account-1' AND mailbox_id='inbox'")) == 0);
    CHECK(textScalar(database, QStringLiteral("SELECT status FROM offline_mailbox_scopes WHERE "
                                              "account_id='account-1' AND mailbox_id='inbox'")) ==
          QStringLiteral("paused"));
    CHECK_FALSE(resyncRequested);
    CHECK(textScalar(database,
                     QStringLiteral("SELECT status FROM mail_vault_projection_jobs WHERE "
                                    "account_id='account-1' AND email_id='shared-archive' AND "
                                    "mailbox_id='archive' AND operation='metadata' ORDER BY job_id "
                                    "DESC LIMIT 1")) == QStringLiteral("pending"));
    REQUIRE(publisher.changes.size() == 1);
}

TEST_CASE("developer body clear batches large reclaimable object cleanup",
          "[app][developer-maintenance][bodies][batch]")
{
    ensureApplication();
    auto context = database();
    seedBase(context.connection);
    auto& database = context.connection.database();
    javelin::jmap::cache::RawMessageSourceRepository sources{context.connection};
    std::uint64_t expectedBytes = 0;
    for (int index = 0; index < 130; ++index)
    {
        const QString emailId = QStringLiteral("batch-%1").arg(index);
        const QString blobId = QStringLiteral("blob-%1").arg(index);
        seedEmail(database, emailId, blobId, QStringLiteral("inbox"));
        const QByteArray payload = QStringLiteral("body-%1").arg(index).toUtf8();
        expectedBytes += static_cast<std::uint64_t>(payload.size());
        REQUIRE_FALSE(sources
                          .upsert("account-1", {.emailId = emailId.toStdString(),
                                                .blobId = blobId.toStdString(),
                                                .payload = payload})
                          .has_value());
    }

    RecordingPublisher publisher;
    javelin::app::MailboxMaintenanceRegistry registry;
    WorkScheduler scheduler{context.connection, nullptr, std::chrono::milliseconds{0}};
    const auto vault = javelin::jmap::cache::MailVault::forDatabase(context.connection);
    DeveloperMaintenanceService maintenance{
        context.directory.filePath(QStringLiteral("cache.sqlite3")), vault.rootPath(), registry,
        publisher, scheduler};
    const auto result = QCoro::waitFor(maintenance.clearMailboxCache({
        .accountId = QStringLiteral("account-1"),
        .mailboxId = QStringLiteral("inbox"),
        .kind = DeveloperMailboxCacheKind::Bodies,
        .offlinePolicy = DeveloperOfflineClearPolicy::Disable,
    }));
    const auto completed = waitForTerminalJob(scheduler, queuedJobId(result));
    REQUIRE(completed.status == WorkStatus::Complete);
    CHECK(completed.progress.completedBytes == expectedBytes);
    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM mail_vault_email_refs WHERE "
                                          "account_id='account-1'")) == 0);
    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM mail_vault_objects")) == 0);
    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM mail_vault_projection_jobs WHERE "
                                          "account_id='account-1' AND mailbox_id='inbox'")) == 0);
}

TEST_CASE("developer body clear defers leased objects for later vault collection",
          "[app][developer-maintenance][bodies][lease]")
{
    ensureApplication();
    auto context = database();
    seedBase(context.connection);
    auto& database = context.connection.database();
    seedEmail(database, QStringLiteral("leased-inbox"), QStringLiteral("blob-leased"),
              QStringLiteral("inbox"));

    const QByteArray payload = QByteArrayLiteral("leased body");
    javelin::jmap::cache::RawMessageSourceRepository sources{context.connection};
    REQUIRE_FALSE(
        sources
            .upsert("account-1",
                    {.emailId = "leased-inbox", .blobId = "blob-leased", .payload = payload})
            .has_value());

    QSqlQuery objectQuery{database};
    REQUIRE(objectQuery.exec(QStringLiteral(
        "SELECT o.content_hash,o.relative_path,o.size FROM mail_vault_objects o JOIN "
        "mail_vault_email_refs r ON r.content_hash=o.content_hash WHERE "
        "r.account_id='account-1' AND r.email_id='leased-inbox'")));
    REQUIRE(objectQuery.next());
    const javelin::jmap::cache::MailVaultObject object{
        .contentHash = objectQuery.value(0).toString().toStdString(),
        .relativePath = objectQuery.value(1).toString(),
        .size = objectQuery.value(2).toULongLong(),
    };
    objectQuery.finish();
    const auto vault = javelin::jmap::cache::MailVault::forDatabase(context.connection);
    auto leaseResult = vault.acquireLease(object);
    auto* lease = std::get_if<javelin::jmap::cache::MailVaultLease>(&leaseResult);
    REQUIRE(lease != nullptr);
    REQUIRE(lease->isValid());

    RecordingPublisher publisher;
    javelin::app::MailboxMaintenanceRegistry registry;
    WorkScheduler scheduler{context.connection, nullptr, std::chrono::milliseconds{0}};
    DeveloperMaintenanceService maintenance{
        context.directory.filePath(QStringLiteral("cache.sqlite3")), vault.rootPath(), registry,
        publisher, scheduler};
    const auto result = QCoro::waitFor(maintenance.clearMailboxCache({
        .accountId = QStringLiteral("account-1"),
        .mailboxId = QStringLiteral("inbox"),
        .kind = DeveloperMailboxCacheKind::Bodies,
        .offlinePolicy = DeveloperOfflineClearPolicy::Disable,
    }));
    const auto completed = waitForTerminalJob(scheduler, queuedJobId(result));
    REQUIRE(completed.status == WorkStatus::Complete);
    CHECK(completed.progress.completedBytes == 0);
    CHECK(completed.progress.detail.contains(QStringLiteral("remains in use")));
    CHECK(QFileInfo::exists(QDir{vault.rootPath()}.filePath(object.relativePath)));
    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM mail_vault_email_refs WHERE "
                                          "account_id='account-1' AND email_id='leased-inbox'")) ==
          0);
    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM mail_vault_objects WHERE "
                                          "content_hash='%1'")
                               .arg(QString::fromStdString(object.contentHash))) == 1);

    leaseResult = javelin::jmap::cache::MailVaultError{.message = QStringLiteral("released")};
    const auto evicted = sources.evictUnretained(10);
    const auto* evictedCount = std::get_if<std::size_t>(&evicted);
    REQUIRE(evictedCount != nullptr);
    CHECK(*evictedCount == 1);
    CHECK_FALSE(QFileInfo::exists(QDir{vault.rootPath()}.filePath(object.relativePath)));
}

TEST_CASE("mailbox cache cleanup is visible and resumes after daemon restart",
          "[app][developer-maintenance][background][recovery]")
{
    ensureApplication();
    auto context = database();
    seedBase(context.connection);
    auto& database = context.connection.database();
    seedEmail(database, QStringLiteral("restart-message"), QStringLiteral("restart-blob"),
              QStringLiteral("inbox"));

    const QByteArray payload = QByteArrayLiteral("restart-safe body");
    javelin::jmap::cache::RawMessageSourceRepository sources{context.connection};
    REQUIRE_FALSE(
        sources
            .upsert("account-1",
                    {.emailId = "restart-message", .blobId = "restart-blob", .payload = payload})
            .has_value());
    const auto vault = javelin::jmap::cache::MailVault::forDatabase(context.connection);
    const QString bodyPath = textScalar(
        database, QStringLiteral("SELECT relative_path FROM mail_vault_objects o JOIN "
                                 "mail_vault_email_refs r ON r.content_hash=o.content_hash WHERE "
                                 "r.account_id='account-1' AND r.email_id='restart-message'"));
    const QString contentHash = textScalar(
        database, QStringLiteral("SELECT content_hash FROM mail_vault_email_refs WHERE "
                                 "account_id='account-1' AND email_id='restart-message'"));
    REQUIRE(QFileInfo::exists(QDir{vault.rootPath()}.filePath(bodyPath)));

    RecordingPublisher publisher;
    javelin::app::MailboxMaintenanceRegistry registry;
    QString jobId;
    {
        WorkScheduler scheduler{context.connection, nullptr, std::chrono::hours{1}};
        DeveloperMaintenanceService maintenance{
            context.directory.filePath(QStringLiteral("cache.sqlite3")), vault.rootPath(), registry,
            publisher, scheduler};
        const auto result = QCoro::waitFor(maintenance.clearMailboxCache({
            .accountId = QStringLiteral("account-1"),
            .mailboxId = QStringLiteral("inbox"),
            .kind = DeveloperMailboxCacheKind::Bodies,
            .offlinePolicy = DeveloperOfflineClearPolicy::Disable,
        }));
        jobId = queuedJobId(result);

        const auto listed = scheduler.list();
        const auto* jobs = std::get_if<std::vector<WorkRecord>>(&listed);
        REQUIRE(jobs != nullptr);
        const auto queued = std::ranges::find(*jobs, jobId.toStdString(), &WorkRecord::jobId);
        REQUIRE(queued != jobs->end());
        CHECK(queued->status == WorkStatus::Queued);
        CHECK(queued->checkpointJson.contains(QStringLiteral("mailbox_cache_cleanup")));
        CHECK(queued->checkpointJson.contains(QStringLiteral("inbox")));

        execute(database,
                QStringLiteral("INSERT INTO mail_vault_projection_jobs(account_id,email_id,"
                               "mailbox_id,content_hash,operation) VALUES('account-1',"
                               "'restart-message','inbox','%1','unlink')")
                    .arg(contentHash));
        execute(database,
                QStringLiteral("DELETE FROM mail_vault_mailbox_refs WHERE account_id='account-1' "
                               "AND mailbox_id='inbox'"));
        execute(database,
                QStringLiteral("DELETE FROM mail_vault_email_refs WHERE account_id='account-1' "
                               "AND email_id='restart-message'"));
        execute(database,
                QStringLiteral("UPDATE background_jobs SET status='running' WHERE job_id='%1'")
                    .arg(jobId));

        CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM mail_vault_mailbox_refs WHERE "
                                              "account_id='account-1' AND mailbox_id='inbox'")) ==
              0);
        CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM mail_vault_email_refs WHERE "
                                              "account_id='account-1' AND "
                                              "email_id='restart-message'")) == 0);
        CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM mail_vault_objects WHERE "
                                              "content_hash='%1'")
                                   .arg(contentHash)) == 1);
        CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM mail_vault_projection_jobs "
                                              "WHERE content_hash='%1' AND operation='unlink'")
                                   .arg(contentHash)) == 1);
    }

    CHECK(QFileInfo::exists(QDir{vault.rootPath()}.filePath(bodyPath)));
    {
        WorkScheduler scheduler{context.connection, nullptr, std::chrono::milliseconds{0}};
        const auto recovered = scheduler.find(jobId.toStdString());
        const auto* record = std::get_if<std::optional<WorkRecord>>(&recovered);
        REQUIRE(record != nullptr);
        REQUIRE(record->has_value());
        CHECK((*record)->status == WorkStatus::Queued);

        DeveloperMaintenanceService maintenance{
            context.directory.filePath(QStringLiteral("cache.sqlite3")), vault.rootPath(), registry,
            publisher, scheduler};
        const auto completed = waitForTerminalJob(scheduler, jobId);
        REQUIRE(completed.status == WorkStatus::Complete);
        CHECK(completed.progress.completedBytes == static_cast<std::uint64_t>(payload.size()));
    }

    CHECK_FALSE(QFileInfo::exists(QDir{vault.rootPath()}.filePath(bodyPath)));
    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM mail_vault_objects WHERE "
                                          "content_hash='%1'")
                               .arg(contentHash)) == 0);
    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM mail_vault_projection_jobs WHERE "
                                          "content_hash='%1'")
                               .arg(contentHash)) == 0);
    REQUIRE(publisher.changes.size() == 1);
    CHECK(publisher.changes.front().mailboxIds == QStringList{QStringLiteral("inbox")});
}
