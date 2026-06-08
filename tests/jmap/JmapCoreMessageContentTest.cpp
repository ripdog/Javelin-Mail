#include "FixtureReader.h"
#include "jmap/JmapCore.h"
#include "jmap/api/MethodEnvelope.h"
#include "jmap/api/SessionParser.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/RawMessageSourceRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/ThreadRepository.h"
#include "jmap/domain/MailEntityParsers.h"
#include "jmap/sync/PendingActions.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUrlQuery>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
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

    class FakeTransport final : public javelin::jmap::api::AbstractTransport
    {
      public:
        std::vector<javelin::jmap::api::HttpRequest> requests;
        std::vector<javelin::jmap::api::TransportResult> queuedResults;
        std::function<javelin::jmap::api::TransportResult(const javelin::jmap::api::HttpRequest&)>
            responseFactory;

        [[nodiscard]] QCoro::Task<javelin::jmap::api::TransportResult>
        send(const javelin::jmap::api::HttpRequest& request) override
        {
            requests.push_back(request);
            if (responseFactory)
            {
                co_return responseFactory(request);
            }
            REQUIRE_FALSE(queuedResults.empty());
            auto result = std::move(queuedResults.front());
            queuedResults.erase(queuedResults.begin());
            co_return result;
        }
    };

    [[nodiscard]] QString makeConnectionName()
    {
        static int counter = 0;
        ++counter;
        return QStringLiteral("javelin-core-message-content-%1").arg(counter);
    }

    struct TestDatabaseContext
    {
        QTemporaryDir temporaryDir;
        javelin::jmap::cache::DatabaseConnection connection;
    };

    [[nodiscard]] TestDatabaseContext makeDatabaseContext()
    {
        TestDatabaseContext context;
        REQUIRE(context.temporaryDir.isValid());

        auto result = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = makeConnectionName(),
            .databasePath = context.temporaryDir.filePath(QStringLiteral("cache.sqlite3")),
        });
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
        {
            FAIL(error->message.toStdString());
        }

        context.connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(result));
        return context;
    }

    [[nodiscard]] javelin::jmap::api::Session loadSessionFixture()
    {
        const auto parsed = javelin::jmap::api::parseSession(
            javelin::tests::loadFixture("jmap/session/basic_session.json"), {
                                                                                .mail = true,
                                                                                .submission = false,
                                                                            });
        REQUIRE(parsed.ok());
        REQUIRE(parsed.session.has_value());
        return *parsed.session;
    }

    [[nodiscard]] javelin::jmap::domain::Email loadEmailFixture()
    {
        const auto parsed = javelin::jmap::domain::parseEmail(
            javelin::tests::loadFixture("jmap/entities/email.json"));
        REQUIRE(parsed.ok());
        REQUIRE(parsed.value.has_value());
        return *parsed.value;
    }

    void seedAccount(javelin::jmap::cache::DatabaseConnection& connection)
    {
        QSqlQuery query{connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO accounts (account_id, email_address, session_url, is_primary) "
            "VALUES (:account_id, :email_address, :session_url, :is_primary)"));
        query.bindValue(QStringLiteral(":account_id"), QStringLiteral("account-1"));
        query.bindValue(QStringLiteral(":email_address"), QStringLiteral("alice@example.com"));
        query.bindValue(QStringLiteral(":session_url"),
                        QStringLiteral("https://mail.example.com/.well-known/jmap"));
        query.bindValue(QStringLiteral(":is_primary"), 1);
        REQUIRE(query.exec());
    }

    void seedEmail(javelin::jmap::cache::DatabaseConnection& connection)
    {
        QSqlQuery query{connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO emails ("
            "account_id, email_id, thread_id, blob_id, received_at, subject, preview, "
            "mailbox_ids_json, keywords_json, has_attachment, size"
            ") VALUES ("
            ":account_id, :email_id, :thread_id, :blob_id, :received_at, :subject, "
            ":preview, :mailbox_ids_json, :keywords_json, :has_attachment, :size)"));
        query.bindValue(QStringLiteral(":account_id"), QStringLiteral("u1"));
        query.bindValue(QStringLiteral(":email_id"), QStringLiteral("eml-1"));
        query.bindValue(QStringLiteral(":thread_id"), QStringLiteral("thr-1"));
        query.bindValue(QStringLiteral(":blob_id"), QStringLiteral("blob-root"));
        query.bindValue(QStringLiteral(":received_at"), QStringLiteral("2026-04-05T11:22:33Z"));
        query.bindValue(QStringLiteral(":subject"), QStringLiteral("Inline image"));
        query.bindValue(QStringLiteral(":preview"), QStringLiteral("Preview"));
        query.bindValue(QStringLiteral(":mailbox_ids_json"), QStringLiteral("[]"));
        query.bindValue(QStringLiteral(":keywords_json"), QStringLiteral("{}"));
        query.bindValue(QStringLiteral(":has_attachment"), 1);
        query.bindValue(QStringLiteral(":size"), 512);
        REQUIRE(query.exec());
    }

} // namespace

TEST_CASE("JmapCore refreshMessageContent caches raw message sources",
          "[jmap][core][message-content]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessionRepository{databaseContext.connection};
    const auto session = loadSessionFixture();
    REQUIRE_FALSE(sessionRepository.replace("u1", session).has_value());
    seedEmail(databaseContext.connection);

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArrayLiteral("Subject: Inline image\r\n"
                                  "Content-Type: multipart/related; boundary=\"b\"\r\n"
                                  "\r\n"
                                  "--b\r\n"
                                  "Content-Type: text/html; charset=utf-8\r\n"
                                  "\r\n"
                                  "<img src=\"cid:chart@cid\">\r\n"
                                  "--b\r\n"
                                  "Content-Type: image/png\r\n"
                                  "Content-ID: <chart@cid>\r\n"
                                  "\r\n"
                                  "PNGDATA\r\n"
                                  "--b--\r\n"),
    });

    javelin::jmap::JmapCore core{databaseContext.connection, transport};
    const auto result = QCoro::waitFor(core.refreshMessageContent(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1", "eml-1"));

    if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
    {
        FAIL(error->message.toStdString());
    }

    REQUIRE(std::holds_alternative<javelin::jmap::MessageContentRefreshSummary>(result));
    REQUIRE(transport.requests.size() == 1);
    CHECK(transport.requests.front().method == javelin::jmap::api::HttpMethod::Get);
    CHECK(transport.requests.front().url.scheme() == QStringLiteral("https"));
    CHECK(transport.requests.front().url.host() == QStringLiteral("mail.example.com"));
    CHECK(transport.requests.front().url.path() ==
          QStringLiteral("/jmap/download/u1/blob-root/Inline image.eml"));
    const QUrlQuery downloadQuery{transport.requests.front().url};
    const auto downloadType = downloadQuery.queryItemValue(QStringLiteral("type"));
    CHECK_FALSE(downloadType.isEmpty());
    CHECK(downloadType.contains(QStringLiteral("message")));
    CHECK(transport.requests.front().headers.front().value == "Bearer access-token");

    javelin::jmap::cache::RawMessageSourceRepository sourceRepository{databaseContext.connection};
    const auto sourceResult = sourceRepository.find("u1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::RawMessageSource>>(
        sourceResult));
    const auto& source =
        std::get<std::optional<javelin::jmap::cache::RawMessageSource>>(sourceResult);
    REQUIRE(source.has_value());
    CHECK(source->blobId == "blob-root");
    CHECK(source->payload.contains(QByteArrayLiteral("<img src=\"cid:chart@cid\">")));
}

TEST_CASE("JmapCore searchMessages uses Email/query text filters and caches thread results",
          "[jmap][core][search]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessionRepository{databaseContext.connection};
    const auto session = loadSessionFixture();
    REQUIRE_FALSE(sessionRepository.replace("u1", session).has_value());

    FakeTransport transport;
    transport.responseFactory = [](const javelin::jmap::api::HttpRequest& request)
    {
        const auto envelope = javelin::jmap::api::parseRequestEnvelope(request.body.toStdString());
        REQUIRE(envelope.ok());
        REQUIRE(envelope.value.has_value());
        REQUIRE(envelope.value->methodCalls.size() == 4);

        javelin::jmap::api::ResponseEnvelope response{
            .methodResponses =
                {
                    javelin::jmap::api::MethodInvocation{
                        .name = "Email/query",
                        .arguments =
                            R"({"accountId":"u1","queryState":"query-state-1","canCalculateChanges":true,"position":0,"ids":["eml-2"],"total":1})",
                        .callId = envelope.value->methodCalls[0].callId,
                    },
                    javelin::jmap::api::MethodInvocation{
                        .name = "Email/get",
                        .arguments =
                            R"({"accountId":"u1","state":"email-state-1","list":[{"id":"eml-2","blobId":"blob-2","threadId":"thr-1","mailboxIds":{"mbx-inbox":true},"keywords":{"$flagged":true},"size":4096,"receivedAt":"2026-04-06T11:22:33Z","sentAt":"2026-04-06T11:21:00Z","hasAttachment":true,"subject":"Quarterly update","from":[{"name":"Alice Sender","email":"alice@example.com"}],"to":[{"name":"Bob Recipient","email":"bob@example.com"}],"cc":[],"bcc":[],"replyTo":[],"preview":"Quarterly preview"}],"notFound":[]})",
                        .callId = envelope.value->methodCalls[1].callId,
                    },
                    javelin::jmap::api::MethodInvocation{
                        .name = "Thread/get",
                        .arguments =
                            R"({"accountId":"u1","state":"thread-state-1","list":[{"id":"thr-1","emailIds":["eml-1","eml-2"]}],"notFound":[]})",
                        .callId = envelope.value->methodCalls[2].callId,
                    },
                    javelin::jmap::api::MethodInvocation{
                        .name = "Email/get",
                        .arguments =
                            R"({"accountId":"u1","state":"email-state-1","list":[{"id":"eml-1","blobId":"blob-1","threadId":"thr-1","mailboxIds":{"mbx-inbox":true},"keywords":{"$seen":true},"size":1024,"receivedAt":"2026-04-05T11:22:33Z","sentAt":"2026-04-05T11:21:00Z","hasAttachment":false,"subject":"Earlier note","from":[{"name":"Alice Sender","email":"alice@example.com"}],"to":[{"name":"Bob Recipient","email":"bob@example.com"}],"cc":[],"bcc":[],"replyTo":[],"preview":"Earlier preview"},{"id":"eml-2","blobId":"blob-2","threadId":"thr-1","mailboxIds":{"mbx-inbox":true},"keywords":{"$flagged":true},"size":4096,"receivedAt":"2026-04-06T11:22:33Z","sentAt":"2026-04-06T11:21:00Z","hasAttachment":true,"subject":"Quarterly update","from":[{"name":"Alice Sender","email":"alice@example.com"}],"to":[{"name":"Bob Recipient","email":"bob@example.com"}],"cc":[],"bcc":[],"replyTo":[],"preview":"Quarterly preview"}],"notFound":[]})",
                        .callId = envelope.value->methodCalls[3].callId,
                    },
                },
            .createdIds = std::unordered_map<std::string, std::string>{},
            .sessionState = "session-state-2",
        };
        const auto body = javelin::jmap::api::serializeResponseEnvelope(response);
        REQUIRE(body.has_value());
        return javelin::jmap::api::TransportResult{javelin::jmap::api::HttpResponse{
            .statusCode = 200,
            .body = QByteArray::fromStdString(*body),
        }};
    };

    javelin::jmap::JmapCore core{databaseContext.connection, transport};
    const auto result = QCoro::waitFor(core.searchMessages(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1", "quarterly", 0));

    if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
    {
        FAIL(error->message.toStdString());
    }

    REQUIRE(std::holds_alternative<javelin::jmap::MessageSearchSummary>(result));
    const auto& summary = std::get<javelin::jmap::MessageSearchSummary>(result);
    CHECK(summary.accountId == "u1");
    CHECK(summary.query == "quarterly");
    CHECK(summary.total == std::optional<std::size_t>{1});
    REQUIRE(summary.results.size() == 1);
    CHECK(summary.results.front().emailId == "eml-2");
    CHECK(summary.results.front().threadId == "thr-1");
    CHECK(summary.results.front().threadMessageCount == 2);
    CHECK(summary.results.front().isUnread);
    CHECK(summary.results.front().isFlagged);
    REQUIRE(summary.results.front().from.has_value());
    CHECK(summary.results.front().from->email == "alice@example.com");

    REQUIRE(transport.requests.size() == 1);
    const auto requestBody = QString::fromUtf8(transport.requests.front().body);
    CHECK(requestBody.contains(QStringLiteral("\"text\":\"quarterly\"")));
    CHECK(requestBody.contains(QStringLiteral("\"calculateTotal\":true")));

    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    const auto emailResult = emailRepository.find("u1", "eml-2");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(emailResult));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Email>>(emailResult).has_value());

    javelin::jmap::cache::ThreadRepository threadRepository{databaseContext.connection};
    const auto threadResult = threadRepository.find("u1", "thr-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Thread>>(threadResult));
    const auto& cachedThread = std::get<std::optional<javelin::jmap::domain::Thread>>(threadResult);
    REQUIRE(cachedThread.has_value());
    CHECK(cachedThread->emailIds == std::vector<std::string>{"eml-1", "eml-2"});
}

TEST_CASE("JmapCore queues archive and delete mailbox moves as pending actions",
          "[jmap][core][pending-actions]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    auto email = loadEmailFixture();
    email.id = "eml-1";
    email.threadId = "thr-1";
    email.mailboxIds = {"mbx-inbox"};
    email.keywords = {};

    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    REQUIRE_FALSE(emailRepository.replaceAll("account-1", {email}).has_value());

    FakeTransport transport;
    javelin::jmap::JmapCore core{databaseContext.connection, transport};

    const auto archiveResult =
        core.queueArchiveEmail("account-1", "eml-1", "mbx-inbox", "mbx-archive");
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(archiveResult));

    const auto archivedEmailResult = emailRepository.find("account-1", "eml-1");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(archivedEmailResult));
    const auto& archivedEmail =
        std::get<std::optional<javelin::jmap::domain::Email>>(archivedEmailResult);
    REQUIRE(archivedEmail.has_value());
    CHECK(archivedEmail->mailboxIds == std::vector<std::string>{"mbx-archive"});

    const auto deleteResult =
        core.queueDeleteEmail("account-1", "eml-1", "mbx-archive", "mbx-trash");
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(deleteResult));

    const auto deletedEmailResult = emailRepository.find("account-1", "eml-1");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(deletedEmailResult));
    const auto& deletedEmail =
        std::get<std::optional<javelin::jmap::domain::Email>>(deletedEmailResult);
    REQUIRE(deletedEmail.has_value());
    CHECK(deletedEmail->mailboxIds == std::vector<std::string>{"mbx-trash"});

    javelin::jmap::sync::PendingActionRepository pendingActionRepository{
        databaseContext.connection};
    const auto pendingResult = pendingActionRepository.listForEmail("account-1", "eml-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::PendingActionRecord>>(
        pendingResult));
    const auto& records =
        std::get<std::vector<javelin::jmap::sync::PendingActionRecord>>(pendingResult);
    REQUIRE(records.size() == 2);
    CHECK(std::any_of(records.cbegin(), records.cend(),
                      [](const auto& record)
                      {
                          return record.emailPatch.addMailboxIds ==
                                     std::vector<std::string>{"mbx-archive"} &&
                                 record.emailPatch.removeMailboxIds ==
                                     std::vector<std::string>{"mbx-inbox"};
                      }));
    CHECK(std::any_of(records.cbegin(), records.cend(),
                      [](const auto& record)
                      {
                          return record.emailPatch.addMailboxIds ==
                                     std::vector<std::string>{"mbx-trash"} &&
                                 record.emailPatch.removeMailboxIds ==
                                     std::vector<std::string>{"mbx-archive"};
                      }));
}

TEST_CASE("JmapCore queues read keyword mutations as pending actions",
          "[jmap][core][pending-actions]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    auto email = loadEmailFixture();
    email.id = "eml-1";
    email.threadId = "thr-1";
    email.mailboxIds = {"mbx-inbox"};
    email.keywords = {};

    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    REQUIRE_FALSE(emailRepository.replaceAll("account-1", {email}).has_value());

    FakeTransport transport;
    javelin::jmap::JmapCore core{databaseContext.connection, transport};

    const auto markReadResult = core.queueMarkEmailRead("account-1", "eml-1");
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(markReadResult));

    const auto readEmailResult = emailRepository.find("account-1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(readEmailResult));
    const auto& readEmail = std::get<std::optional<javelin::jmap::domain::Email>>(readEmailResult);
    REQUIRE(readEmail.has_value());
    CHECK(readEmail->keywords == std::vector<std::string>{"$seen"});

    const auto markUnreadResult = core.queueMarkEmailUnread("account-1", "eml-1");
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(markUnreadResult));

    const auto unreadEmailResult = emailRepository.find("account-1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(unreadEmailResult));
    const auto& unreadEmail =
        std::get<std::optional<javelin::jmap::domain::Email>>(unreadEmailResult);
    REQUIRE(unreadEmail.has_value());
    CHECK(unreadEmail->keywords.empty());
}

TEST_CASE("JmapCore downloadAttachment reads attachment payloads from cached raw source",
          "[jmap][core][message-content]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessionRepository{databaseContext.connection};
    const auto session = loadSessionFixture();
    REQUIRE_FALSE(sessionRepository.replace("u1", session).has_value());
    seedEmail(databaseContext.connection);

    javelin::jmap::cache::RawMessageSourceRepository sourceRepository{databaseContext.connection};
    REQUIRE_FALSE(sourceRepository
                      .upsert("u1",
                              {
                                  .emailId = "eml-1",
                                  .blobId = "blob-root",
                                  .payload = QByteArrayLiteral(
                                      "Subject: Attachment\r\n"
                                      "Content-Type: multipart/mixed; boundary=\"b\"\r\n"
                                      "\r\n"
                                      "--b\r\n"
                                      "Content-Type: text/plain; charset=utf-8\r\n"
                                      "\r\n"
                                      "Body\r\n"
                                      "--b\r\n"
                                      "Content-Type: application/pdf; name=\"report.pdf\"\r\n"
                                      "Content-Disposition: attachment; filename=\"report.pdf\"\r\n"
                                      "\r\n"
                                      "%PDF-data\r\n"
                                      "--b--\r\n"),
                              })
                      .has_value());

    FakeTransport transport;
    javelin::jmap::JmapCore core{databaseContext.connection, transport};
    const auto result = QCoro::waitFor(core.downloadAttachment(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1", "eml-1", "2"));

    if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
    {
        FAIL(error->message.toStdString());
    }

    REQUIRE(std::holds_alternative<javelin::jmap::AttachmentDownload>(result));
    const auto& download = std::get<javelin::jmap::AttachmentDownload>(result);
    CHECK(download.partId == "2");
    CHECK(download.name == std::optional<std::string>{"report.pdf"});
    CHECK(download.mediaType == "application/pdf");
    CHECK(download.payload == QByteArrayLiteral("%PDF-data"));
    CHECK(download.usedCachedInlinePayload);
    CHECK(transport.requests.empty());
}

TEST_CASE("JmapCore downloadAttachment reads inline payloads from cached raw source",
          "[jmap][core][message-content]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessionRepository{databaseContext.connection};
    const auto session = loadSessionFixture();
    REQUIRE_FALSE(sessionRepository.replace("u1", session).has_value());
    seedEmail(databaseContext.connection);

    javelin::jmap::cache::RawMessageSourceRepository sourceRepository{databaseContext.connection};
    REQUIRE_FALSE(sourceRepository
                      .upsert("u1",
                              {
                                  .emailId = "eml-1",
                                  .blobId = "blob-root",
                                  .payload = QByteArrayLiteral(
                                      "Subject: Inline image\r\n"
                                      "Content-Type: multipart/related; boundary=\"b\"\r\n"
                                      "\r\n"
                                      "--b\r\n"
                                      "Content-Type: text/html; charset=utf-8\r\n"
                                      "\r\n"
                                      "<img src=\"cid:chart@cid\">\r\n"
                                      "--b\r\n"
                                      "Content-Type: image/png; name=\"chart.png\"\r\n"
                                      "Content-Disposition: inline; filename=\"chart.png\"\r\n"
                                      "Content-ID: <chart@cid>\r\n"
                                      "\r\n"
                                      "PNGDATA\r\n"
                                      "--b--\r\n"),
                              })
                      .has_value());

    FakeTransport transport;
    javelin::jmap::JmapCore core{databaseContext.connection, transport};
    const auto result = QCoro::waitFor(core.downloadAttachment(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1", "eml-1", "2"));

    if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
    {
        FAIL(error->message.toStdString());
    }

    REQUIRE(std::holds_alternative<javelin::jmap::AttachmentDownload>(result));
    const auto& download = std::get<javelin::jmap::AttachmentDownload>(result);
    CHECK(download.partId == "2");
    CHECK(download.name == std::optional<std::string>{"chart.png"});
    CHECK(download.mediaType == "image/png");
    CHECK(download.payload == QByteArrayLiteral("PNGDATA"));
    CHECK(download.usedCachedInlinePayload);
    CHECK(transport.requests.empty());
}

TEST_CASE("JmapCore submits queued mailbox mutations through Email/set",
          "[jmap][core][pending-actions]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessionRepository{databaseContext.connection};
    const auto session = loadSessionFixture();
    REQUIRE_FALSE(sessionRepository.replace("u1", session).has_value());

    auto email = loadEmailFixture();
    email.id = "eml-1";
    email.threadId = "thr-1";
    email.mailboxIds = {"mbx-inbox"};
    email.keywords = {"$seen"};

    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    REQUIRE_FALSE(emailRepository.replaceAll("u1", {email}).has_value());

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            R"({"methodResponses":[["Email/set",{"accountId":"u1","oldState":"email-state-1","newState":"email-state-2","updated":{"eml-1":null},"notUpdated":{}},"queued-email-set"]],"createdIds":{},"sessionState":"session-state-2"})",
    });

    javelin::jmap::JmapCore core{databaseContext.connection, transport};
    const auto queuedResult = core.queueArchiveEmail("u1", "eml-1", "mbx-inbox", "mbx-archive");
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(queuedResult));

    const auto submitResult = QCoro::waitFor(core.submitPendingEmailMutations(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1"));

    if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&submitResult))
    {
        FAIL(error->message.toStdString());
    }

    REQUIRE(std::holds_alternative<javelin::jmap::SubmittedEmailMutations>(submitResult));
    const auto& summary = std::get<javelin::jmap::SubmittedEmailMutations>(submitResult);
    CHECK(summary.attemptedEmailCount == 1);
    CHECK(summary.updatedEmailCount == 1);
    CHECK(summary.failedEmailCount == 0);

    REQUIRE(transport.requests.size() == 1);
    CHECK(transport.requests.front().method == javelin::jmap::api::HttpMethod::Post);
    CHECK(transport.requests.front().body.contains("\"Email/set\""));
    CHECK(transport.requests.front().body.contains("\"mbx-archive\":true"));
    CHECK_FALSE(transport.requests.front().body.contains("\"mbx-inbox\":true"));

    const auto refreshedEmailResult = emailRepository.find("u1", "eml-1");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(refreshedEmailResult));
    const auto& refreshedEmail =
        std::get<std::optional<javelin::jmap::domain::Email>>(refreshedEmailResult);
    REQUIRE(refreshedEmail.has_value());
    CHECK(refreshedEmail->mailboxIds == std::vector<std::string>{"mbx-archive"});
    CHECK(refreshedEmail->keywords == std::vector<std::string>{"$seen"});

    javelin::jmap::sync::PendingActionRepository pendingActionRepository{
        databaseContext.connection};
    const auto pendingResult = pendingActionRepository.listForEmail("u1", "eml-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::PendingActionRecord>>(
        pendingResult));
    CHECK(std::get<std::vector<javelin::jmap::sync::PendingActionRecord>>(pendingResult).empty());
}

TEST_CASE("JmapCore submits queued read keyword mutations through Email/set",
          "[jmap][core][pending-actions]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessionRepository{databaseContext.connection};
    const auto session = loadSessionFixture();
    REQUIRE_FALSE(sessionRepository.replace("u1", session).has_value());

    auto email = loadEmailFixture();
    email.id = "eml-1";
    email.threadId = "thr-1";
    email.mailboxIds = {"mbx-inbox"};
    email.keywords = {};

    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    REQUIRE_FALSE(emailRepository.replaceAll("u1", {email}).has_value());

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            R"({"methodResponses":[["Email/set",{"accountId":"u1","oldState":"email-state-1","newState":"email-state-2","updated":{"eml-1":null},"notUpdated":{}},"queued-email-set"]],"createdIds":{},"sessionState":"session-state-2"})",
    });

    javelin::jmap::JmapCore core{databaseContext.connection, transport};
    const auto queuedResult = core.queueMarkEmailRead("u1", "eml-1");
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(queuedResult));

    const auto submitResult = QCoro::waitFor(core.submitPendingEmailMutations(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1"));

    if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&submitResult))
    {
        FAIL(error->message.toStdString());
    }

    REQUIRE(std::holds_alternative<javelin::jmap::SubmittedEmailMutations>(submitResult));
    const auto& summary = std::get<javelin::jmap::SubmittedEmailMutations>(submitResult);
    CHECK(summary.attemptedEmailCount == 1);
    CHECK(summary.updatedEmailCount == 1);
    CHECK(summary.failedEmailCount == 0);

    REQUIRE(transport.requests.size() == 1);
    CHECK(transport.requests.front().body.contains("\"Email/set\""));
    CHECK(transport.requests.front().body.contains("\"$seen\":true"));

    const auto refreshedEmailResult = emailRepository.find("u1", "eml-1");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(refreshedEmailResult));
    const auto& refreshedEmail =
        std::get<std::optional<javelin::jmap::domain::Email>>(refreshedEmailResult);
    REQUIRE(refreshedEmail.has_value());
    CHECK(refreshedEmail->keywords == std::vector<std::string>{"$seen"});
}
