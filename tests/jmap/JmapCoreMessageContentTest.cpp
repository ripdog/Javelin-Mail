#include "FixtureReader.h"
#include "jmap/JmapCore.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MethodEnvelope.h"
#include "jmap/api/SessionParser.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/QueryService.h"
#include "jmap/cache/RawMessageSourceRepository.h"
#include "jmap/cache/SearchWindowRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/ThreadRepository.h"
#include "jmap/domain/MailEntityParsers.h"
#include "jmap/search/EmailSearch.h"
#include "jmap/sync/ConsistencyDomain.h"
#include "jmap/sync/EmailMutationJournal.h"

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
        FakeTransport() : methodTransport(*this)
        {
        }

        javelin::jmap::api::HttpJmapMethodTransport methodTransport;
        std::vector<javelin::jmap::api::HttpRequest> requests;
        std::vector<javelin::jmap::api::TransportResult> queuedResults;
        std::function<javelin::jmap::api::TransportResult(const javelin::jmap::api::HttpRequest&)>
            responseFactory;

        [[nodiscard]] QCoro::Task<javelin::jmap::api::TransportResult>
        send(javelin::jmap::api::HttpRequest request) override
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
        query.bindValue(QStringLiteral(":subject"),
                        QStringLiteral("Re: [libsdl-org/SDL] Android: save dialog"));
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

    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto result = QCoro::waitFor(core.refreshMessageContent(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1", "eml-1"));

    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
    {
        FAIL(error->message.toStdString());
    }

    REQUIRE(std::holds_alternative<javelin::jmap::MessageContentRefreshSummary>(result));
    REQUIRE(transport.requests.size() == 1);
    CHECK(transport.requests.front().method == javelin::jmap::api::HttpMethod::Get);
    CHECK(transport.requests.front().url.scheme() == QStringLiteral("https"));
    CHECK(transport.requests.front().url.host() == QStringLiteral("mail.example.com"));
    CHECK(transport.requests.front().url.path() ==
          QStringLiteral("/jmap/download/u1/blob-root/eml-1.eml"));
    CHECK_FALSE(transport.requests.front().url.toString().contains(QStringLiteral("libsdl")));
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

TEST_CASE("JmapCore reports missing message source downloads distinctly",
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
    transport.queuedResults.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::HttpFailure,
        .message = "server replied with status code 404",
        .httpStatus = 404,
    });

    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto result = QCoro::waitFor(core.refreshMessageContent(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1", "eml-1"));

    REQUIRE(std::holds_alternative<javelin::jmap::MessageContentUnavailable>(result));
    const auto& unavailable = std::get<javelin::jmap::MessageContentUnavailable>(result);
    CHECK(unavailable.accountId == "u1");
    CHECK(unavailable.emailId == "eml-1");
    CHECK(unavailable.message.contains(QStringLiteral("HTTP 404")));
}

TEST_CASE("JmapCore full mailbox pages do not request unsafe server previews",
          "[jmap][core][offline]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessions{databaseContext.connection};
    REQUIRE_FALSE(sessions.replace("u1", loadSessionFixture()).has_value());

    FakeTransport transport;
    transport.responseFactory = [](const javelin::jmap::api::HttpRequest& request)
    {
        const auto envelope = javelin::jmap::api::parseRequestEnvelope(request.body.toStdString());
        REQUIRE(envelope.ok());
        REQUIRE(envelope.value.has_value());
        REQUIRE(envelope.value->methodCalls.size() == 2);
        const QString getArguments =
            QString::fromStdString(envelope.value->methodCalls[1].arguments);
        CHECK(getArguments.contains(QStringLiteral("\"subject\"")));
        CHECK(getArguments.contains(QStringLiteral("\"from\"")));
        CHECK_FALSE(getArguments.contains(QStringLiteral("\"preview\"")));

        javelin::jmap::api::ResponseEnvelope response{
            .methodResponses =
                {
                    javelin::jmap::api::MethodInvocation{
                        .name = "Email/query",
                        .arguments =
                            R"({"accountId":"u1","queryState":"query-1","canCalculateChanges":true,"position":0,"ids":["eml-1"],"total":1})",
                        .callId = envelope.value->methodCalls[0].callId,
                    },
                    javelin::jmap::api::MethodInvocation{
                        .name = "Email/get",
                        .arguments =
                            R"({"accountId":"u1","state":"email-1","list":[{"id":"eml-1","blobId":"blob-1","threadId":"thr-1","mailboxIds":{"mbx-inbox":true},"keywords":{},"size":42,"receivedAt":"2026-07-21T01:00:00Z","hasAttachment":false,"subject":"Cached safely","from":[{"email":"alice@example.com"}],"to":[],"cc":[],"bcc":[],"replyTo":[]}],"notFound":[]})",
                        .callId = envelope.value->methodCalls[1].callId,
                    },
                },
            .createdIds = std::unordered_map<std::string, std::string>{},
            .sessionState = "session-2",
        };
        const auto body = javelin::jmap::api::serializeResponseEnvelope(response);
        REQUIRE(body.has_value());
        return javelin::jmap::api::TransportResult{javelin::jmap::api::HttpResponse{
            .statusCode = 200,
            .body = QByteArray::fromStdString(*body),
        }};
    };

    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto result = QCoro::waitFor(core.materializeFullMailboxPage(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1", "mbx-inbox", 0, 250, std::nullopt));
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        FAIL(error->message.toStdString());
    const auto& page = std::get<javelin::jmap::FullMailboxPage>(result);
    REQUIRE(page.emails.size() == 1);
    CHECK(page.emails.front().subject == std::optional<std::string>{"Cached safely"});
    CHECK_FALSE(page.emails.front().preview.has_value());
}

TEST_CASE("JmapCore caches message content from junk and trash mailboxes",
          "[jmap][core][message-content]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessionRepository{databaseContext.connection};
    const auto session = loadSessionFixture();
    REQUIRE_FALSE(sessionRepository.replace("u1", session).has_value());
    seedEmail(databaseContext.connection);

    QSqlQuery mailbox{databaseContext.connection.database()};
    mailbox.prepare(QStringLiteral("INSERT INTO mailboxes (account_id, mailbox_id, name, role) "
                                   "VALUES (:account_id, :mailbox_id, :name, :role)"));
    mailbox.bindValue(QStringLiteral(":account_id"), QStringLiteral("u1"));
    mailbox.bindValue(QStringLiteral(":mailbox_id"), QStringLiteral("mbx-junk"));
    mailbox.bindValue(QStringLiteral(":name"), QStringLiteral("Junk"));
    mailbox.bindValue(QStringLiteral(":role"), QStringLiteral("junk"));
    REQUIRE(mailbox.exec());
    mailbox.bindValue(QStringLiteral(":account_id"), QStringLiteral("u1"));
    mailbox.bindValue(QStringLiteral(":mailbox_id"), QStringLiteral("mbx-trash"));
    mailbox.bindValue(QStringLiteral(":name"), QStringLiteral("Trash"));
    mailbox.bindValue(QStringLiteral(":role"), QStringLiteral("trash"));
    REQUIRE(mailbox.exec());

    QSqlQuery assignMailbox{databaseContext.connection.database()};
    assignMailbox.prepare(QStringLiteral(
        "UPDATE emails SET mailbox_ids_json = :mailbox_ids WHERE account_id = :account_id "
        "AND email_id = :email_id"));
    assignMailbox.bindValue(QStringLiteral(":mailbox_ids"),
                            QStringLiteral("[\"mbx-junk\",\"mbx-trash\"]"));
    assignMailbox.bindValue(QStringLiteral(":account_id"), QStringLiteral("u1"));
    assignMailbox.bindValue(QStringLiteral(":email_id"), QStringLiteral("eml-1"));
    REQUIRE(assignMailbox.exec());

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArrayLiteral("Subject: Junk message\r\n"
                                  "Content-Type: text/plain; charset=utf-8\r\n"
                                  "\r\n"
                                  "Readable junk body\r\n"),
    });

    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto result = QCoro::waitFor(core.refreshMessageContent(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1", "eml-1"));
    REQUIRE(std::holds_alternative<javelin::jmap::MessageContentRefreshSummary>(result));

    javelin::jmap::cache::RawMessageSourceRepository sourceRepository{databaseContext.connection};
    const auto sourceResult = sourceRepository.find("u1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::RawMessageSource>>(
        sourceResult));
    CHECK(
        std::get<std::optional<javelin::jmap::cache::RawMessageSource>>(sourceResult).has_value());
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

    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto result = QCoro::waitFor(core.searchMessages(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1", "quarterly", 0));

    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
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

    javelin::jmap::cache::QueryService queryService{databaseContext.connection};
    const auto cachedWindowResult = queryService.loadSearchWindow(
        "u1",
        javelin::jmap::search::cacheKey(
            {.text = "quarterly"},
            {.property = javelin::jmap::query::EmailListSortProperty::ReceivedAt,
             .direction = javelin::jmap::query::EmailListSortDirection::Descending}),
        0, 100);
    const auto* cachedWindow =
        std::get_if<std::optional<javelin::jmap::cache::SearchWindowPage>>(&cachedWindowResult);
    REQUIRE(cachedWindow != nullptr);
    REQUIRE(cachedWindow->has_value());
    CHECK((*cachedWindow)->total == std::optional<std::size_t>{1});
    REQUIRE((*cachedWindow)->items.size() == 1);
    CHECK((*cachedWindow)->items.front().emailId == "eml-2");

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

TEST_CASE("JmapCore queues archive and delete mailbox moves as mutations",
          "[jmap][core][mutation-journal]")
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
    const std::string inboxQueryKey = "mailbox:mbx-inbox|sort:receivedAt:desc|collapseThreads:true";
    javelin::jmap::cache::MailboxWindowRepository mailboxWindows{databaseContext.connection};
    REQUIRE_FALSE(mailboxWindows
                      .replace({
                          .accountId = "account-1",
                          .mailboxId = "mbx-inbox",
                          .queryKey = inboxQueryKey,
                          .requestedOffset = 0,
                          .requestedLimit = 100,
                          .position = 0,
                          .returnedLimit = 100,
                          .total = 1,
                          .queryState = "query-state-1",
                          .emailIds = {"eml-1"},
                      })
                      .has_value());

    FakeTransport transport;
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};

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

    javelin::jmap::cache::QueryService queryService{databaseContext.connection};
    const auto optimisticInbox = queryService.loadMailboxWindow("account-1", inboxQueryKey, 0, 100);
    const auto* inboxPage =
        std::get_if<std::optional<javelin::jmap::cache::MailboxWindowPage>>(&optimisticInbox);
    REQUIRE(inboxPage != nullptr);
    REQUIRE(inboxPage->has_value());
    CHECK_FALSE((*inboxPage)->isAuthoritative);
    CHECK((*inboxPage)->items.empty());
    const auto optimisticArchive =
        queryService.listMailboxMessages("account-1", "mbx-archive", 100);
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MessageListItem>>(
        optimisticArchive));
    const auto& archiveItems =
        std::get<std::vector<javelin::jmap::cache::MessageListItem>>(optimisticArchive);
    REQUIRE(archiveItems.size() == 1);
    CHECK(archiveItems.front().emailId == "eml-1");

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

    javelin::jmap::sync::EmailMutationJournal emailMutationJournal{databaseContext.connection};
    const auto pendingResult = emailMutationJournal.listForEmail("account-1", "eml-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::EmailMutationRecord>>(
        pendingResult));
    const auto& records =
        std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(pendingResult);
    REQUIRE(records.size() == 2);
    CHECK(std::any_of(records.cbegin(), records.cend(),
                      [](const auto& record)
                      {
                          return record.patch.addMailboxIds ==
                                     std::vector<std::string>{"mbx-archive"} &&
                                 record.patch.removeMailboxIds ==
                                     std::vector<std::string>{"mbx-inbox"};
                      }));
    CHECK(std::any_of(records.cbegin(), records.cend(),
                      [](const auto& record)
                      {
                          return record.patch.addMailboxIds ==
                                     std::vector<std::string>{"mbx-trash"} &&
                                 record.patch.removeMailboxIds ==
                                     std::vector<std::string>{"mbx-archive"};
                      }));
}

TEST_CASE("JmapCore permanently destroys queued emails through Email/set",
          "[jmap][core][mutation-journal]")
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
    email.mailboxIds = {"mbx-trash"};

    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    REQUIRE_FALSE(emailRepository.replaceAll("u1", {email}).has_value());

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            R"({"methodResponses":[["Email/set",{"accountId":"u1","oldState":"email-state-1","newState":"email-state-2","updated":{},"destroyed":["eml-1"],"notUpdated":{},"notDestroyed":{}},"queued-email-set"]],"createdIds":{},"sessionState":"session-state-2"})",
    });

    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto queuedResult = core.queueDestroyEmail("u1", "eml-1");
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(queuedResult));

    const auto optimisticEmailResult = emailRepository.find("u1", "eml-1");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(optimisticEmailResult));
    const auto& optimisticEmail =
        std::get<std::optional<javelin::jmap::domain::Email>>(optimisticEmailResult);
    REQUIRE(optimisticEmail.has_value());
    CHECK(optimisticEmail->mailboxIds.empty());

    const auto submitResult = QCoro::waitFor(core.submitPendingEmailMutations(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1"));

    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&submitResult))
    {
        FAIL(error->message.toStdString());
    }

    const auto& summary = std::get<javelin::jmap::SubmittedEmailMutations>(submitResult);
    CHECK(summary.attemptedEmailCount == 1);
    CHECK(summary.updatedEmailCount == 1);
    CHECK(summary.failedEmailCount == 0);

    REQUIRE(transport.requests.size() == 1);
    CHECK(transport.requests.front().body.contains("\"destroy\":[\"eml-1\"]"));

    const auto deletedEmailResult = emailRepository.find("u1", "eml-1");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(deletedEmailResult));
    CHECK_FALSE(
        std::get<std::optional<javelin::jmap::domain::Email>>(deletedEmailResult).has_value());
}

TEST_CASE("JmapCore queues mailbox copies as mutations", "[jmap][core][mutation-journal]")
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
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};

    const auto copyResult = core.queueCopyEmail("account-1", "eml-1", "mbx-inbox", "mbx-projects");
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(copyResult));

    const auto copiedEmailResult = emailRepository.find("account-1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(copiedEmailResult));
    const auto& copiedEmail =
        std::get<std::optional<javelin::jmap::domain::Email>>(copiedEmailResult);
    REQUIRE(copiedEmail.has_value());
    CHECK(copiedEmail->mailboxIds == std::vector<std::string>{"mbx-inbox", "mbx-projects"});

    javelin::jmap::sync::EmailMutationJournal emailMutationJournal{databaseContext.connection};
    const auto pendingResult = emailMutationJournal.listForEmail("account-1", "eml-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::EmailMutationRecord>>(
        pendingResult));
    const auto& records =
        std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(pendingResult);
    REQUIRE(records.size() == 1);
    CHECK(records.front().patch.addMailboxIds == std::vector<std::string>{"mbx-projects"});
    CHECK(records.front().patch.removeMailboxIds.empty());
}

TEST_CASE("JmapCore queues exact mailbox patches as mutations", "[jmap][core][mutation-journal]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    auto email = loadEmailFixture();
    email.id = "eml-1";
    email.threadId = "thr-1";
    email.mailboxIds = {"mbx-inbox", "mbx-projects"};
    email.keywords = {};

    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    REQUIRE_FALSE(emailRepository.replaceAll("account-1", {email}).has_value());

    javelin::jmap::cache::MailboxWindowRepository mailboxWindows{databaseContext.connection};
    for (const auto& mailboxId : email.mailboxIds)
    {
        const auto key = "mailbox:" + mailboxId + "|sort:receivedAt:desc|collapseThreads:true";
        REQUIRE_FALSE(mailboxWindows
                          .replace({
                              .accountId = "account-1",
                              .mailboxId = mailboxId,
                              .queryKey = key,
                              .requestedOffset = 0,
                              .requestedLimit = 100,
                              .position = 0,
                              .returnedLimit = 100,
                              .total = 1,
                              .queryState = "state-1",
                              .emailIds = {"eml-1"},
                          })
                          .has_value());
    }
    javelin::jmap::cache::SearchWindowRepository searchWindows{databaseContext.connection};
    REQUIRE_FALSE(searchWindows
                      .replace({
                          .accountId = "account-1",
                          .queryKey = "search-key",
                          .offset = 0,
                          .limit = 100,
                          .position = 0,
                          .returnedLimit = 100,
                          .total = 1,
                          .queryState = "state-1",
                          .emailIds = {"eml-1"},
                      })
                      .has_value());

    FakeTransport transport;
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};

    const auto moveResult = core.queueEmailMailboxMutation(
        "account-1", javelin::jmap::EmailMailboxMutation{
                         .emailId = "eml-1",
                         .addMailboxIds = {"mbx-archive"},
                         .removeMailboxIds = {"mbx-inbox", "mbx-projects"},
                     });
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(moveResult));

    const auto movedEmailResult = emailRepository.find("account-1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(movedEmailResult));
    const auto& movedEmail =
        std::get<std::optional<javelin::jmap::domain::Email>>(movedEmailResult);
    REQUIRE(movedEmail.has_value());
    CHECK(movedEmail->mailboxIds == std::vector<std::string>{"mbx-archive"});
    for (const auto& mailboxId : std::vector<std::string>{"mbx-inbox", "mbx-projects"})
    {
        const auto key = "mailbox:" + mailboxId + "|sort:receivedAt:desc|collapseThreads:true";
        const auto window = mailboxWindows.find("account-1", key, 0, 100);
        const auto* cached =
            std::get_if<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(&window);
        REQUIRE(cached != nullptr);
        REQUIRE(cached->has_value());
        CHECK_FALSE((*cached)->isAuthoritative);
    }
    const auto searchWindow = searchWindows.find("account-1", "search-key", 0, 100);
    const auto* cachedSearch =
        std::get_if<std::optional<javelin::jmap::cache::SearchWindowRecord>>(&searchWindow);
    REQUIRE(cachedSearch != nullptr);
    CHECK_FALSE(cachedSearch->has_value());
}

TEST_CASE("JmapCore queues read keyword mutations as mutations", "[jmap][core][mutation-journal]")
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
    const std::string queryKey = "mailbox:mbx-inbox|sort:receivedAt:desc|collapseThreads:true";
    javelin::jmap::cache::MailboxWindowRepository mailboxWindows{databaseContext.connection};
    REQUIRE_FALSE(mailboxWindows
                      .replace({
                          .accountId = "account-1",
                          .mailboxId = "mbx-inbox",
                          .queryKey = queryKey,
                          .requestedOffset = 0,
                          .requestedLimit = 100,
                          .position = 0,
                          .returnedLimit = 100,
                          .total = 1,
                          .queryState = "query-state-1",
                          .emailIds = {"eml-1"},
                      })
                      .has_value());

    FakeTransport transport;
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};

    const auto markReadResult = core.queueMarkEmailRead("account-1", "eml-1");
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(markReadResult));

    const auto readEmailResult = emailRepository.find("account-1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(readEmailResult));
    const auto& readEmail = std::get<std::optional<javelin::jmap::domain::Email>>(readEmailResult);
    REQUIRE(readEmail.has_value());
    CHECK(readEmail->keywords == std::vector<std::string>{"$seen"});

    javelin::jmap::cache::QueryService queryService{databaseContext.connection};
    const auto readPageResult = queryService.loadMailboxWindow("account-1", queryKey, 0, 100);
    const auto* readPage =
        std::get_if<std::optional<javelin::jmap::cache::MailboxWindowPage>>(&readPageResult);
    REQUIRE(readPage != nullptr);
    REQUIRE(readPage->has_value());
    REQUIRE((*readPage)->items.size() == 1);
    CHECK_FALSE((*readPage)->items.front().isUnread);

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
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto result = QCoro::waitFor(core.downloadAttachment(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1", "eml-1", "2"));

    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
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

TEST_CASE("JmapCore loads message source from the cached raw payload",
          "[jmap][core][message-content]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessionRepository{databaseContext.connection};
    REQUIRE_FALSE(sessionRepository.replace("u1", loadSessionFixture()).has_value());
    seedEmail(databaseContext.connection);

    const QByteArray payload = QByteArrayLiteral("Subject: Cached source\r\n\r\nBody\r\n");
    javelin::jmap::cache::RawMessageSourceRepository sourceRepository{databaseContext.connection};
    REQUIRE_FALSE(sourceRepository
                      .upsert("u1",
                              {
                                  .emailId = "eml-1",
                                  .blobId = "blob-root",
                                  .payload = payload,
                              })
                      .has_value());

    FakeTransport transport;
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto result = QCoro::waitFor(core.loadCachedMessageSource("u1", "eml-1"));

    REQUIRE(std::holds_alternative<javelin::jmap::MessageSourceDownload>(result));
    const auto& source = std::get<javelin::jmap::MessageSourceDownload>(result);
    CHECK(source.accountId == "u1");
    CHECK(source.emailId == "eml-1");
    CHECK(source.blobId == "blob-root");
    CHECK(source.payload == payload);
    CHECK(transport.requests.empty());
}

TEST_CASE("JmapCore rejects a missing or stale cached message source",
          "[jmap][core][message-content]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessionRepository{databaseContext.connection};
    REQUIRE_FALSE(sessionRepository.replace("u1", loadSessionFixture()).has_value());
    seedEmail(databaseContext.connection);

    javelin::jmap::cache::RawMessageSourceRepository sourceRepository{databaseContext.connection};
    REQUIRE_FALSE(sourceRepository
                      .upsert("u1",
                              {
                                  .emailId = "eml-1",
                                  .blobId = "stale-blob",
                                  .payload = QByteArrayLiteral("Stale source"),
                              })
                      .has_value());

    FakeTransport transport;
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto result = QCoro::waitFor(core.loadCachedMessageSource("u1", "eml-1"));

    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
    CHECK(std::get<javelin::jmap::OperationError>(result).message.contains(
        QStringLiteral("not cached locally")));
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
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto result = QCoro::waitFor(core.downloadAttachment(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1", "eml-1", "2"));

    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
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
          "[jmap][core][mutation-journal]")
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

    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto queuedResult = core.queueArchiveEmail("u1", "eml-1", "mbx-inbox", "mbx-archive");
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(queuedResult));

    const auto submitResult = QCoro::waitFor(core.submitPendingEmailMutations(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1"));

    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&submitResult))
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
    CHECK(transport.requests.front().body.contains("\"mailboxIds/mbx-archive\":true"));
    CHECK(transport.requests.front().body.contains("\"mailboxIds/mbx-inbox\":null"));
    CHECK_FALSE(transport.requests.front().body.contains("\"mailboxIds\":{"));
    CHECK_FALSE(transport.requests.front().body.contains("\"mbx-inbox\":true"));

    const auto refreshedEmailResult = emailRepository.find("u1", "eml-1");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(refreshedEmailResult));
    const auto& refreshedEmail =
        std::get<std::optional<javelin::jmap::domain::Email>>(refreshedEmailResult);
    REQUIRE(refreshedEmail.has_value());
    CHECK(refreshedEmail->mailboxIds == std::vector<std::string>{"mbx-archive"});
    CHECK(refreshedEmail->keywords == std::vector<std::string>{"$seen"});

    javelin::jmap::sync::EmailMutationJournal emailMutationJournal{databaseContext.connection};
    const auto pendingResult = emailMutationJournal.listForEmail("u1", "eml-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::EmailMutationRecord>>(
        pendingResult));
    CHECK(std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(pendingResult).empty());
}

TEST_CASE("JmapCore atomically rolls back accepted Email mutation projection failures",
          "[jmap][core][mutation-journal][consistency]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessionRepository{databaseContext.connection};
    REQUIRE_FALSE(sessionRepository.replace("u1", loadSessionFixture()).has_value());

    auto email = loadEmailFixture();
    email.id = "eml-1";
    email.threadId = "thr-1";
    email.mailboxIds = {"mbx-inbox"};
    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    REQUIRE_FALSE(emailRepository.replaceAll("u1", {email}).has_value());

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            R"({"methodResponses":[["Email/set",{"accountId":"u1","oldState":"email-state-1","newState":"email-state-2","updated":{"eml-1":null},"notUpdated":{}},"queued-email-set"]],"createdIds":{},"sessionState":"session-state-2"})",
    });

    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto queuedResult = core.queueArchiveEmail("u1", "eml-1", "mbx-inbox", "mbx-archive");
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(queuedResult));
    const auto mutationId = std::get<javelin::jmap::QueuedEmailMutation>(queuedResult).mutationId;

    QSqlQuery trigger{databaseContext.connection.database()};
    REQUIRE(trigger.exec(
        QStringLiteral("CREATE TRIGGER reject_email_acceptance BEFORE UPDATE ON emails BEGIN "
                       "SELECT RAISE(ABORT,'acceptance rejected'); END")));

    const auto submitResult = QCoro::waitFor(core.submitPendingEmailMutations(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1"));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(submitResult));

    javelin::jmap::sync::MutationJournalRepository journal{databaseContext.connection};
    const auto mutationResult = journal.find(mutationId);
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::sync::MutationRecord>>(mutationResult));
    const auto& mutation =
        std::get<std::optional<javelin::jmap::sync::MutationRecord>>(mutationResult);
    REQUIRE(mutation.has_value());
    CHECK(mutation->status == javelin::jmap::sync::MutationStatus::InFlight);

    javelin::jmap::sync::ConsistencyDomainRepository consistency{databaseContext.connection};
    const auto generation = consistency.mutationGeneration({
        .accountId = "u1",
        .dataType = "Email",
    });
    REQUIRE(std::holds_alternative<std::uint64_t>(generation));
    CHECK(std::get<std::uint64_t>(generation) == 0);

    const auto cachedResult = emailRepository.find("u1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(cachedResult));
    const auto& cached = std::get<std::optional<javelin::jmap::domain::Email>>(cachedResult);
    REQUIRE(cached.has_value());
    CHECK(cached->mailboxIds == std::vector<std::string>{"mbx-archive"});
}

TEST_CASE("JmapCore restores rejected Email mutations immediately",
          "[jmap][core][mutation-journal][consistency]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessionRepository{databaseContext.connection};
    REQUIRE_FALSE(sessionRepository.replace("u1", loadSessionFixture()).has_value());

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
            R"({"methodResponses":[["Email/set",{"accountId":"u1","oldState":"email-state-1","newState":"email-state-1","updated":{},"notUpdated":{"eml-1":{"type":"forbidden"}}},"queued-email-set"]],"createdIds":{},"sessionState":"session-state-2"})",
    });

    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto queued = core.queueArchiveEmail("u1", "eml-1", "mbx-inbox", "mbx-archive");
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(queued));
    const auto mutationId = std::get<javelin::jmap::QueuedEmailMutation>(queued).mutationId;
    const auto queuedUnread = core.queueMarkEmailUnread("u1", "eml-1");
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(queuedUnread));

    const auto optimistic = emailRepository.find("u1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(optimistic));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Email>>(optimistic).has_value());
    CHECK(std::get<std::optional<javelin::jmap::domain::Email>>(optimistic)->mailboxIds ==
          std::vector<std::string>{"mbx-archive"});
    CHECK(std::get<std::optional<javelin::jmap::domain::Email>>(optimistic)->keywords.empty());

    const auto submitted = QCoro::waitFor(core.submitPendingEmailMutations(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1"));
    REQUIRE(std::holds_alternative<javelin::jmap::SubmittedEmailMutations>(submitted));
    CHECK(std::get<javelin::jmap::SubmittedEmailMutations>(submitted).failedEmailCount == 1);

    const auto restored = emailRepository.find("u1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(restored));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Email>>(restored).has_value());
    CHECK(std::get<std::optional<javelin::jmap::domain::Email>>(restored)->mailboxIds ==
          std::vector<std::string>{"mbx-inbox"});
    CHECK(std::get<std::optional<javelin::jmap::domain::Email>>(restored)->keywords ==
          std::vector<std::string>{"$seen"});

    javelin::jmap::sync::MutationJournalRepository journal{databaseContext.connection};
    const auto mutation = journal.find(mutationId);
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::sync::MutationRecord>>(mutation));
    REQUIRE(std::get<std::optional<javelin::jmap::sync::MutationRecord>>(mutation).has_value());
    CHECK(std::get<std::optional<javelin::jmap::sync::MutationRecord>>(mutation)->status ==
          javelin::jmap::sync::MutationStatus::Rejected);
}

TEST_CASE("JmapCore submits queued read keyword mutations through Email/set",
          "[jmap][core][mutation-journal]")
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

    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto queuedResult = core.queueMarkEmailRead("u1", "eml-1");
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(queuedResult));

    const auto submitResult = QCoro::waitFor(core.submitPendingEmailMutations(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1"));

    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&submitResult))
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
    CHECK(transport.requests.front().body.contains("\"keywords/$seen\":true"));
    CHECK_FALSE(transport.requests.front().body.contains("\"keywords\":{"));

    const auto refreshedEmailResult = emailRepository.find("u1", "eml-1");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(refreshedEmailResult));
    const auto& refreshedEmail =
        std::get<std::optional<javelin::jmap::domain::Email>>(refreshedEmailResult);
    REQUIRE(refreshedEmail.has_value());
    CHECK(refreshedEmail->keywords == std::vector<std::string>{"$seen"});
}

TEST_CASE("JmapCore preserves ambiguous Email mutation outcomes for reconciliation",
          "[jmap][core][mutation-journal]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessionRepository{databaseContext.connection};
    REQUIRE_FALSE(sessionRepository.replace("u1", loadSessionFixture()).has_value());

    auto email = loadEmailFixture();
    email.id = "eml-1";
    email.threadId = "thr-1";
    email.mailboxIds = {"mbx-inbox"};
    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    REQUIRE_FALSE(emailRepository.replaceAll("u1", {email}).has_value());

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
        .message = "Connection closed after request dispatch",
    });

    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto queued = core.queueArchiveEmail("u1", "eml-1", "mbx-inbox", "mbx-archive");
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(queued));

    const auto submitted = QCoro::waitFor(core.submitPendingEmailMutations(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1"));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(submitted));

    javelin::jmap::sync::EmailMutationJournal journal{databaseContext.connection};
    const auto mutations = journal.listForEmail("u1", "eml-1");
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::sync::EmailMutationRecord>>(mutations));
    const auto& records =
        std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(mutations);
    REQUIRE(records.size() == 1);
    CHECK(records.front().status == javelin::jmap::sync::MutationStatus::Unknown);

    const auto effectiveEmail = emailRepository.find("u1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(effectiveEmail));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Email>>(effectiveEmail).has_value());
    CHECK(std::get<std::optional<javelin::jmap::domain::Email>>(effectiveEmail)->mailboxIds ==
          std::vector<std::string>{"mbx-archive"});
}
