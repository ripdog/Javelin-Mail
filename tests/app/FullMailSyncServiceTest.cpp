#include "app/FullMailSyncService.h"
#include "app/MailIndexService.h"
#include "app/WorkScheduler.h"
#include "jmap/JmapCore.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/Session.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/Database.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailSearchIndex.h"
#include "jmap/cache/RawMessageSourceRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/domain/MailEntities.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>

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
            static char appName[] = "full-mail-sync-service-test";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    class RecordingResourceTransport final : public javelin::jmap::api::AbstractTransport
    {
      public:
        std::vector<javelin::jmap::api::HttpRequest> requests;
        std::function<void(const javelin::jmap::api::HttpRequest&)> beforeReply;
        int statusCode = 200;

        [[nodiscard]] QCoro::Task<javelin::jmap::api::TransportResult>
        send(javelin::jmap::api::HttpRequest request) override
        {
            requests.push_back(request);
            if (beforeReply)
                beforeReply(requests.back());
            co_return javelin::jmap::api::HttpResponse{
                .statusCode = statusCode,
                .body = QByteArrayLiteral(
                    "From: sender@example.test\r\nTo: user@example.test\r\nSubject: Saved\r\n\r\n"
                    "offline body\r\n"),
            };
        }
    };

    class RejectingMethodTransport final : public javelin::jmap::api::JmapMethodTransport
    {
      public:
        std::size_t calls = 0;

        [[nodiscard]] QCoro::Task<javelin::jmap::api::JmapMethodTransportResult>
        call(javelin::jmap::api::JmapMethodRequest request) override
        {
            Q_UNUSED(request);
            ++calls;
            co_return javelin::jmap::api::TransportError{
                .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
                .message = "Unexpected JMAP method request during body hydration",
            };
        }
    };

    [[nodiscard]] QString makeConnectionName()
    {
        static int counter = 0;
        return QStringLiteral("full-mail-sync-service-%1").arg(++counter);
    }

    struct TestDatabase
    {
        QTemporaryDir directory;
        javelin::jmap::cache::DatabaseConnection connection;
    };

    [[nodiscard]] TestDatabase makeDatabase()
    {
        TestDatabase database;
        REQUIRE(database.directory.isValid());
        auto opened = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = makeConnectionName(),
            .databasePath = database.directory.filePath(QStringLiteral("cache.sqlite3")),
        });
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
            FAIL(error->message.toStdString());
        database.connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
        return database;
    }

    [[nodiscard]] javelin::jmap::api::Session testSession()
    {
        javelin::jmap::api::Session session{
            .username = "user@example.test",
            .apiUrl = "https://example.test/jmap",
            .downloadUrl = "https://example.test/download/{accountId}/{blobId}/{name}?type={type}",
            .uploadUrl = "https://example.test/upload/{accountId}",
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
            .accounts = {},
            .primaryAccounts = {.mailAccountId = "account-1",
                                .submissionAccountId = std::nullopt,
                                .contactsAccountId = std::nullopt,
                                .calendarsAccountId = std::nullopt,
                                .sieveAccountId = std::nullopt},
        };
        session.accounts.emplace("account-1",
                                 javelin::jmap::api::Account{
                                     .id = "account-1",
                                     .name = "Test Account",
                                     .isPersonal = true,
                                     .isReadOnly = false,
                                     .accountCapabilities = {.mail = true,
                                                             .submission = std::nullopt,
                                                             .contacts = std::nullopt,
                                                             .calendars = std::nullopt,
                                                             .sieve = false},
                                 });
        return session;
    }

    void seedAccount(javelin::jmap::cache::DatabaseConnection& connection)
    {
        javelin::jmap::cache::SessionRepository sessions{connection};
        REQUIRE_FALSE(sessions.replace("account-1", testSession()).has_value());
        QSqlQuery mailbox{connection.database()};
        REQUIRE(mailbox.exec(QStringLiteral(
            "INSERT INTO mailboxes(account_id,mailbox_id,name,role,total_emails,total_threads,"
            "is_subscribed) VALUES('account-1','archive','Archive','archive',0,0,1)")));
    }

    [[nodiscard]] javelin::jmap::domain::Email
    email(std::string id, std::string blobId, std::string receivedAt, const std::uint64_t size)
    {
        return {
            .id = std::move(id),
            .blobId = std::move(blobId),
            .threadId = "thread-" + blobId,
            .mailboxIds = {"archive"},
            .keywords = {},
            .size = size,
            .receivedAt = std::move(receivedAt),
            .sentAt = std::nullopt,
            .messageId = {},
            .inReplyTo = {},
            .references = {},
            .hasAttachment = false,
            .subject = "Offline test",
            .from = {},
            .to = {},
            .cc = {},
            .bcc = {},
            .replyTo = {},
            .preview = std::nullopt,
        };
    }

    void upsertEmail(javelin::jmap::cache::DatabaseConnection& connection,
                     javelin::jmap::domain::Email value)
    {
        javelin::jmap::cache::EmailRepository emails{connection};
        REQUIRE_FALSE(emails.upsertMany("account-1", {std::move(value)}).has_value());
    }

    void cacheBody(javelin::jmap::cache::DatabaseConnection& connection, const std::string& emailId,
                   const std::string& blobId)
    {
        javelin::jmap::cache::RawMessageSourceRepository sources{connection};
        REQUIRE_FALSE(
            sources
                .upsert("account-1",
                        {.emailId = emailId,
                         .blobId = blobId,
                         .payload = QByteArrayLiteral(
                             "From: cached@example.test\r\nSubject: Cached\r\n\r\nbody\r\n")})
                .has_value());
    }

    [[nodiscard]] std::string fullSyncJobId()
    {
        const QByteArray key = QByteArrayLiteral("account-1") + '\0' + QByteArrayLiteral("archive");
        return "full-mailbox-" +
               QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex().toStdString();
    }

    [[nodiscard]] std::string mailIndexJobId()
    {
        return "mail-index-" +
               QCryptographicHash::hash(QByteArrayLiteral("account-1"), QCryptographicHash::Sha256)
                   .toHex()
                   .toStdString();
    }

    void seedFetchingScope(javelin::jmap::cache::DatabaseConnection& connection,
                           const std::uint64_t generation,
                           const std::vector<std::string>& membershipEmailIds)
    {
        QSqlQuery scope{connection.database()};
        scope.prepare(QStringLiteral(
            "INSERT INTO offline_mailbox_scopes(account_id,mailbox_id,desired,status,query_state,"
            "email_state,expected_total,completed_total,generation) VALUES('account-1','archive',1,"
            "'fetching','query-state','email-state',:total,:total,:generation)"));
        scope.bindValue(QStringLiteral(":total"),
                        static_cast<qulonglong>(membershipEmailIds.size()));
        scope.bindValue(QStringLiteral(":generation"), static_cast<qulonglong>(generation));
        REQUIRE(scope.exec());

        QSqlQuery membership{connection.database()};
        membership.prepare(QStringLiteral(
            "INSERT INTO offline_mailbox_membership(account_id,mailbox_id,email_id,generation,"
            "position) VALUES('account-1','archive',:email,:generation,:position)"));
        std::uint64_t position = 0;
        for (const auto& emailId : membershipEmailIds)
        {
            membership.bindValue(QStringLiteral(":email"), QString::fromStdString(emailId));
            membership.bindValue(QStringLiteral(":generation"),
                                 static_cast<qulonglong>(generation));
            membership.bindValue(QStringLiteral(":position"), static_cast<qulonglong>(position++));
            REQUIRE(membership.exec());
        }

        QSqlQuery job{connection.database()};
        job.prepare(QStringLiteral(
            "INSERT INTO background_jobs(job_id,account_id,kind,priority,status,title,detail,"
            "completed_units,total_units,checkpoint_json) VALUES(:id,'account-1','full_mail_sync',"
            ":priority,'running','Download Archive','Downloading complete messages',1,:total,"
            "'{}')"));
        job.bindValue(QStringLiteral(":id"), QString::fromStdString(fullSyncJobId()));
        job.bindValue(QStringLiteral(":priority"),
                      static_cast<int>(javelin::app::WorkPriority::Bulk));
        job.bindValue(QStringLiteral(":total"), static_cast<qulonglong>(membershipEmailIds.size()));
        REQUIRE(job.exec());
    }

    [[nodiscard]] javelin::app::FullSyncAccountConfiguration configuration()
    {
        return {
            .settings = {.connectionId = "connection-1",
                         .revision = 0,
                         .displayName = "Test",
                         .sessionUrl = "https://example.test/session",
                         .loginEmail = "user@example.test",
                         .apiKey = "token",
                         .refreshToken = {},
                         .tokenEndpoint = {},
                         .oauthClientId = {},
                         .oauthIssuer = {},
                         .oauthResource = {},
                         .oauthScope = {},
                         .revocationEndpoint = {},
                         .registrationClientUri = {},
                         .registrationAccessToken = {}},
            .accountId = "account-1",
            .mailboxIds = {"archive"},
        };
    }

    [[nodiscard]] bool waitUntil(const std::function<bool()>& predicate,
                                 const int timeoutMilliseconds = 5000)
    {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < timeoutMilliseconds)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(1);
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        return predicate();
    }

    [[nodiscard]] QString scopeStatus(javelin::jmap::cache::DatabaseConnection& connection)
    {
        QSqlQuery query{connection.database()};
        REQUIRE(query.exec(QStringLiteral(
            "SELECT status FROM offline_mailbox_scopes WHERE account_id='account-1' AND "
            "mailbox_id='archive'")));
        REQUIRE(query.next());
        return query.value(0).toString();
    }

    [[nodiscard]] std::uint64_t scalar(javelin::jmap::cache::DatabaseConnection& connection,
                                       const QString& sql)
    {
        QSqlQuery query{connection.database()};
        REQUIRE(query.exec(sql));
        REQUIRE(query.next());
        return query.value(0).toULongLong();
    }
} // namespace

TEST_CASE("offline sync does not run for a hidden mailbox", "[app][offline][hidden-mailbox]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabase();
    seedAccount(database.connection);
    QSqlQuery hide{database.connection.database()};
    REQUIRE(hide.exec(
        QStringLiteral("UPDATE mailboxes SET is_subscribed=0 WHERE account_id='account-1' AND "
                       "mailbox_id='archive'")));

    RecordingResourceTransport resources;
    RejectingMethodTransport methods;
    javelin::jmap::JmapCore core{database.connection, resources, methods};
    javelin::app::WorkScheduler scheduler{database.connection, nullptr,
                                          std::chrono::milliseconds{0}};
    javelin::app::MailIndexService indexer{database.connection, scheduler};
    javelin::app::FullMailSyncService service{database.connection, core, scheduler, indexer};
    service.applySettings({configuration()});

    CHECK(scalar(database.connection,
                 QStringLiteral("SELECT COUNT(*) FROM offline_mailbox_scopes WHERE "
                                "account_id='account-1' AND mailbox_id='archive' AND desired=1")) ==
          0);
    const auto job = scheduler.find(fullSyncJobId());
    const auto* record = std::get_if<std::optional<javelin::app::WorkRecord>>(&job);
    REQUIRE(record != nullptr);
    CHECK_FALSE(record->has_value());
    CHECK(methods.calls == 0);
    CHECK(resources.requests.empty());
}

TEST_CASE("offline body hydration resumes after restart without re-enumerating mailbox",
          "[app][offline][restart]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabase();
    seedAccount(database.connection);

    upsertEmail(database.connection, email("email-1", "blob-1", "2026-08-07T10:00:00Z", 100));
    upsertEmail(database.connection, email("email-2", "blob-2", "2026-08-07T08:00:00Z", 200));
    upsertEmail(database.connection,
                email("email-middle", "blob-middle", "2026-08-07T09:00:00Z", 150));
    cacheBody(database.connection, "email-1", "blob-1");
    seedFetchingScope(database.connection, 7, {"email-1", "email-2"});

    RecordingResourceTransport resources;
    RejectingMethodTransport methods;
    javelin::jmap::JmapCore core{database.connection, resources, methods};
    javelin::app::WorkScheduler scheduler{database.connection, nullptr,
                                          std::chrono::milliseconds{0}};
    javelin::app::MailIndexService indexer{database.connection, scheduler};
    javelin::app::FullMailSyncService service{database.connection, core, scheduler, indexer};
    service.applySettings({configuration()});

    REQUIRE(waitUntil(
        [&]()
        {
            const auto job = scheduler.find(fullSyncJobId());
            const auto* record = std::get_if<std::optional<javelin::app::WorkRecord>>(&job);
            return record != nullptr && record->has_value() &&
                   (*record)->status == javelin::app::WorkStatus::Complete;
        }));

    CHECK(methods.calls == 0);
    CHECK(resources.requests.size() == 2);
    CHECK(scopeStatus(database.connection) == QStringLiteral("complete"));
    CHECK(scalar(database.connection,
                 QStringLiteral("SELECT generation FROM offline_mailbox_scopes WHERE "
                                "account_id='account-1' AND mailbox_id='archive'")) == 7);
    CHECK(scalar(database.connection,
                 QStringLiteral("SELECT completed_generation FROM offline_mailbox_scopes WHERE "
                                "account_id='account-1' AND mailbox_id='archive'")) == 7);
    CHECK(scalar(database.connection,
                 QStringLiteral(
                     "SELECT COUNT(*) FROM offline_mailbox_membership WHERE "
                     "account_id='account-1' AND mailbox_id='archive' AND generation=7")) == 3);
    CHECK(scalar(database.connection,
                 QStringLiteral("SELECT COUNT(*) FROM mail_vault_email_refs WHERE "
                                "account_id='account-1' AND email_id IN "
                                "('email-1','email-2','email-middle')")) == 3);
    CHECK(scalar(database.connection,
                 QStringLiteral("SELECT completed_bytes FROM offline_mailbox_scopes WHERE "
                                "account_id='account-1' AND mailbox_id='archive'")) == 450);
}

TEST_CASE("offline body hydration fails instead of retrying an unavailable message forever",
          "[app][offline][unavailable]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabase();
    seedAccount(database.connection);

    upsertEmail(database.connection, email("email-1", "blob-1", "2026-08-07T10:00:00Z", 100));
    seedFetchingScope(database.connection, 8, {"email-1"});

    RecordingResourceTransport resources;
    resources.statusCode = 404;
    RejectingMethodTransport methods;
    javelin::jmap::JmapCore core{database.connection, resources, methods};
    javelin::app::WorkScheduler scheduler{database.connection, nullptr,
                                          std::chrono::milliseconds{0}};
    javelin::app::MailIndexService indexer{database.connection, scheduler};
    javelin::app::FullMailSyncService service{database.connection, core, scheduler, indexer};
    service.applySettings({configuration()});

    REQUIRE(waitUntil(
        [&]()
        {
            const auto job = scheduler.find(fullSyncJobId());
            const auto* record = std::get_if<std::optional<javelin::app::WorkRecord>>(&job);
            return record != nullptr && record->has_value() &&
                   (*record)->status == javelin::app::WorkStatus::Failed;
        }));

    CHECK(methods.calls == 0);
    CHECK(resources.requests.size() == 1);
    CHECK(scopeStatus(database.connection) == QStringLiteral("fetching"));
    CHECK(scalar(database.connection,
                 QStringLiteral("SELECT COUNT(*) FROM mail_vault_email_refs WHERE "
                                "account_id='account-1' AND email_id='email-1'")) == 0);
}

TEST_CASE("offline hydration absorbs mail added while body downloads are running",
          "[app][offline][live-arrival]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabase();
    seedAccount(database.connection);

    upsertEmail(database.connection, email("email-1", "blob-1", "2026-08-07T10:00:00Z", 100));
    upsertEmail(database.connection, email("email-2", "blob-2", "2026-08-07T08:00:00Z", 200));
    cacheBody(database.connection, "email-1", "blob-1");
    seedFetchingScope(database.connection, 9, {"email-1", "email-2"});

    RecordingResourceTransport resources;
    bool insertedMiddleMessage = false;
    resources.beforeReply = [&](const javelin::jmap::api::HttpRequest&)
    {
        if (insertedMiddleMessage)
            return;
        insertedMiddleMessage = true;
        upsertEmail(database.connection,
                    email("email-middle", "blob-middle", "2026-08-07T09:00:00Z", 150));
    };
    RejectingMethodTransport methods;
    javelin::jmap::JmapCore core{database.connection, resources, methods};
    javelin::app::WorkScheduler scheduler{database.connection, nullptr,
                                          std::chrono::milliseconds{0}};
    javelin::app::MailIndexService indexer{database.connection, scheduler};
    javelin::app::FullMailSyncService service{database.connection, core, scheduler, indexer};
    service.applySettings({configuration()});

    REQUIRE(waitUntil(
        [&]()
        {
            const auto job = scheduler.find(fullSyncJobId());
            const auto* record = std::get_if<std::optional<javelin::app::WorkRecord>>(&job);
            return record != nullptr && record->has_value() &&
                   (*record)->status == javelin::app::WorkStatus::Complete;
        }));

    REQUIRE(insertedMiddleMessage);
    CHECK(methods.calls == 0);
    CHECK(resources.requests.size() == 2);
    CHECK(scopeStatus(database.connection) == QStringLiteral("complete"));
    CHECK(scalar(database.connection,
                 QStringLiteral(
                     "SELECT COUNT(*) FROM offline_mailbox_membership WHERE "
                     "account_id='account-1' AND mailbox_id='archive' AND generation=9")) == 3);
    CHECK(scalar(database.connection,
                 QStringLiteral("SELECT COUNT(*) FROM mail_vault_email_refs WHERE "
                                "account_id='account-1' AND email_id='email-middle' AND "
                                "blob_id='blob-middle'")) == 1);
    CHECK(scalar(database.connection,
                 QStringLiteral("SELECT completed_bytes FROM offline_mailbox_scopes WHERE "
                                "account_id='account-1' AND mailbox_id='archive'")) == 450);
}

TEST_CASE("mail indexing crosses a worker batch without retaining pending rows",
          "[app][indexing][batch]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabase();
    seedAccount(database.connection);

    constexpr int messageCount = 130;
    for (int index = 0; index < messageCount; ++index)
    {
        const auto emailId = "indexed-" + std::to_string(index);
        const auto blobId = "blob-indexed-" + std::to_string(index);
        upsertEmail(database.connection, email(emailId, blobId, "2026-08-07T10:00:00Z", 100));
        cacheBody(database.connection, emailId, blobId);
    }

    javelin::app::WorkScheduler scheduler{database.connection, nullptr,
                                          std::chrono::milliseconds{0}};
    javelin::app::MailIndexService indexer{database.connection, scheduler};
    indexer.applyAccounts({"account-1"});

    REQUIRE(waitUntil(
        [&]()
        {
            const auto job = scheduler.find(mailIndexJobId());
            const auto* record = std::get_if<std::optional<javelin::app::WorkRecord>>(&job);
            return record != nullptr && record->has_value() &&
                   (*record)->status == javelin::app::WorkStatus::Complete;
        },
        10000));

    CHECK(scalar(database.connection,
                 QStringLiteral("SELECT COUNT(*) FROM mail_vault_email_refs WHERE "
                                "account_id='account-1' AND indexed_hash=content_hash")) ==
          messageCount);
    CHECK(scalar(database.connection,
                 QStringLiteral("SELECT COUNT(*) FROM mail_vault_email_refs WHERE "
                                "account_id='account-1' AND body_preview='body'")) == messageCount);
    const auto job = scheduler.find(mailIndexJobId());
    const auto* record = std::get_if<std::optional<javelin::app::WorkRecord>>(&job);
    REQUIRE(record != nullptr);
    REQUIRE(record->has_value());
    CHECK((*record)->progress.completedUnits == messageCount);

    javelin::jmap::cache::MailSearchIndex searchIndex{database.connection};
    const auto matches = searchIndex.search("account-1", "body", messageCount + 1);
    REQUIRE(std::holds_alternative<std::vector<std::string>>(matches));
    CHECK(std::get<std::vector<std::string>>(matches).size() == messageCount);
}
