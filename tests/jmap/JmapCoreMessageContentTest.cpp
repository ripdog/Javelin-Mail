#include "FixtureReader.h"
#include "jmap/JmapCore.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MethodEnvelope.h"
#include "jmap/api/SessionParser.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/QueryService.h"
#include "jmap/cache/RawMessageSourceRepository.h"
#include "jmap/cache/SearchWindowRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/cache/ThreadRepository.h"
#include "jmap/domain/MailEntityParsers.h"
#include "jmap/search/EmailSearch.h"
#include "jmap/sync/ConsistencyDomain.h"
#include "jmap/sync/EmailMutationJournal.h"
#include "jmap/sync/MailboxMutationJournal.h"

#include <QCoroFuture>
#include <QCoroTask>

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPromise>
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
        bool invokeDispatched = false;

        [[nodiscard]] QCoro::Task<javelin::jmap::api::TransportResult>
        send(javelin::jmap::api::HttpRequest request) override
        {
            requests.push_back(request);
            if (invokeDispatched && request.dispatched)
                request.dispatched();
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

    class PendingTransport final : public javelin::jmap::api::AbstractTransport
    {
      public:
        PendingTransport() : methodTransport(*this)
        {
            m_promise.start();
        }

        ~PendingTransport() override
        {
            if (!m_completed)
            {
                complete(javelin::jmap::api::TransportError{
                    .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
                    .message = "Pending test transport abandoned",
                });
            }
        }

        [[nodiscard]] QCoro::Task<javelin::jmap::api::TransportResult>
        send(javelin::jmap::api::HttpRequest request) override
        {
            requests.push_back(std::move(request));
            started = true;
            co_return co_await qCoro(m_promise.future()).takeResult();
        }

        void complete(javelin::jmap::api::TransportResult result)
        {
            REQUIRE_FALSE(m_completed);
            m_completed = true;
            m_promise.addResult(std::move(result));
            m_promise.finish();
        }

        javelin::jmap::api::HttpJmapMethodTransport methodTransport;
        std::vector<javelin::jmap::api::HttpRequest> requests;
        bool started = false;

      private:
        QPromise<javelin::jmap::api::TransportResult> m_promise;
        bool m_completed = false;
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

    void seedMailbox(javelin::jmap::cache::DatabaseConnection& connection)
    {
        const auto parsed = javelin::jmap::domain::parseMailbox(
            javelin::tests::loadFixture("jmap/entities/mailbox.json"));
        REQUIRE(parsed.ok());
        REQUIRE(parsed.value.has_value());
        javelin::jmap::cache::MailboxRepository mailboxes{connection};
        REQUIRE_FALSE(mailboxes.replaceAll("u1", {*parsed.value}).has_value());
        javelin::jmap::cache::SyncStateRepository states{connection};
        REQUIRE_FALSE(states
                          .upsert({.accountId = "u1", .objectType = "Mailbox", .queryKey = {}},
                                  "mailbox-state-1")
                          .has_value());
    }

    void seedDeletableMailbox(javelin::jmap::cache::DatabaseConnection& connection,
                              const std::uint64_t totalEmails = 0, const bool mayDelete = true)
    {
        const auto parsed = javelin::jmap::domain::parseMailbox(
            javelin::tests::loadFixture("jmap/entities/mailbox.json"));
        REQUIRE(parsed.ok());
        REQUIRE(parsed.value.has_value());
        auto mailbox = *parsed.value;
        mailbox.totalEmails = totalEmails;
        mailbox.unreadEmails = 0;
        mailbox.totalThreads = totalEmails;
        mailbox.unreadThreads = 0;
        mailbox.myRights.mayDelete = mayDelete;
        javelin::jmap::cache::MailboxRepository mailboxes{connection};
        REQUIRE_FALSE(mailboxes.replaceAll("u1", {mailbox}).has_value());
        javelin::jmap::cache::SyncStateRepository states{connection};
        REQUIRE_FALSE(states
                          .upsert({.accountId = "u1", .objectType = "Mailbox", .queryKey = {}},
                                  "mailbox-state-1")
                          .has_value());
    }

    [[nodiscard]] std::string mailboxCreationId(const javelin::jmap::api::HttpRequest& request)
    {
        const auto document = QJsonDocument::fromJson(request.body);
        REQUIRE(document.isObject());
        const auto calls = document.object().value(QStringLiteral("methodCalls")).toArray();
        REQUIRE_FALSE(calls.isEmpty());
        const auto call = calls.at(0).toArray();
        REQUIRE(call.size() >= 2);
        const auto create = call.at(1).toObject().value(QStringLiteral("create")).toObject();
        REQUIRE(create.size() == 1);
        return create.begin().key().toStdString();
    }

    [[nodiscard]] QByteArray mailboxCreateSuccessEnvelope(const std::string& creationId,
                                                          const std::string_view oldState,
                                                          const std::string_view newState)
    {
        return QStringLiteral(
                   R"({"methodResponses":[["Mailbox/set",{"accountId":"u1","oldState":"%1","newState":"%2","created":{"%3":{"id":"mbx-projects"}},"notCreated":{}},"mailbox-create-set"],["Mailbox/get",{"accountId":"u1","state":"%2","list":[{"id":"mbx-projects","name":"Projects","parentId":null,"role":null,"sortOrder":0,"totalEmails":0,"unreadEmails":0,"totalThreads":0,"unreadThreads":0,"isSubscribed":true,"myRights":{"mayReadItems":true,"mayAddItems":true,"mayRemoveItems":true,"maySetSeen":true,"maySetKeywords":true,"mayCreateChild":true,"mayRename":true,"mayDelete":true,"maySubmit":true}}],"notFound":[]},"mailbox-create-get"]],"createdIds":{"%3":"mbx-projects"},"sessionState":"session-state-2"})")
            .arg(QString::fromStdString(std::string{oldState}),
                 QString::fromStdString(std::string{newState}), QString::fromStdString(creationId))
            .toUtf8();
    }

    [[nodiscard]] javelin::jmap::api::Session mailboxCreateSession()
    {
        auto session = loadSessionFixture();
        session.accounts.at("u1").accountCapabilities.mailDetails =
            javelin::jmap::api::MailAccountCapability{.mayCreateTopLevelMailbox = true};
        return session;
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

TEST_CASE("JmapCore message downloads survive concurrent Email mutations",
          "[jmap][core][message-content][consistency]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessionRepository{databaseContext.connection};
    REQUIRE_FALSE(sessionRepository.replace("u1", loadSessionFixture()).has_value());
    seedEmail(databaseContext.connection);

    PendingTransport transport;
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    std::optional<javelin::jmap::MessageContentRefreshResult> completed;
    QEventLoop completionLoop;
    auto task = core.refreshMessageContent(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1", "eml-1");
    QCoro::connect(std::move(task), QCoreApplication::instance(),
                   [&](javelin::jmap::MessageContentRefreshResult result)
                   {
                       completed = std::move(result);
                       completionLoop.quit();
                   });
    REQUIRE(transport.started);

    javelin::jmap::sync::ConsistencyDomainRepository consistency{databaseContext.connection};
    const auto generation = consistency.advanceMutation({.accountId = "u1", .dataType = "Email"});
    REQUIRE(std::holds_alternative<std::uint64_t>(generation));

    transport.complete(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArrayLiteral("Subject: stale download\r\n\r\nstale body\r\n"),
    });
    if (!completed.has_value())
        completionLoop.exec();

    REQUIRE(completed.has_value());
    const auto* summary = std::get_if<javelin::jmap::MessageContentRefreshSummary>(&*completed);
    REQUIRE(summary != nullptr);
    CHECK_FALSE(summary->usedCachedContent);

    javelin::jmap::cache::RawMessageSourceRepository sources{databaseContext.connection};
    const auto source = sources.find("u1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::RawMessageSource>>(source));
    CHECK(std::get<std::optional<javelin::jmap::cache::RawMessageSource>>(source).has_value());
}

TEST_CASE("JmapCore rejects a download when the message blob changes",
          "[jmap][core][message-content][consistency]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessionRepository{databaseContext.connection};
    REQUIRE_FALSE(sessionRepository.replace("u1", loadSessionFixture()).has_value());
    seedEmail(databaseContext.connection);

    PendingTransport transport;
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    std::optional<javelin::jmap::MessageContentRefreshResult> completed;
    QEventLoop completionLoop;
    auto task = core.refreshMessageContent(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1", "eml-1");
    QCoro::connect(std::move(task), QCoreApplication::instance(),
                   [&](javelin::jmap::MessageContentRefreshResult result)
                   {
                       completed = std::move(result);
                       completionLoop.quit();
                   });
    REQUIRE(transport.started);

    QSqlQuery changedBlob{databaseContext.connection.database()};
    REQUIRE(changedBlob.exec(
        QStringLiteral("UPDATE emails SET blob_id='replacement-blob' WHERE account_id='u1' "
                       "AND email_id='eml-1'")));

    transport.complete(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArrayLiteral("Subject: obsolete download\r\n\r\nold body\r\n"),
    });
    if (!completed.has_value())
        completionLoop.exec();

    REQUIRE(completed.has_value());
    CHECK(std::holds_alternative<javelin::jmap::MessageContentUnavailable>(*completed));

    javelin::jmap::cache::RawMessageSourceRepository sources{databaseContext.connection};
    const auto source = sources.find("u1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::RawMessageSource>>(source));
    CHECK_FALSE(
        std::get<std::optional<javelin::jmap::cache::RawMessageSource>>(source).has_value());
    const auto evicted = sources.evictUnretained();
    REQUIRE(std::holds_alternative<std::size_t>(evicted));
    CHECK(std::get<std::size_t>(evicted) == 1);
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

TEST_CASE("JmapCore searchMessages caches representatives before thread results",
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
        REQUIRE(envelope.value->methodCalls.size() == 2);

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
        "u1", "quarterly", 0, 100, {}, std::nullopt, std::string{"session-query"}));

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
    CHECK_FALSE(summary.results.front().mailboxThreadMessageCount.has_value());
    CHECK_FALSE(summary.results.front().globalThreadMessageCount.has_value());
    CHECK(summary.results.front().isUnread);
    CHECK(summary.results.front().isFlagged);
    REQUIRE(summary.results.front().from.has_value());
    CHECK(summary.results.front().from->email == "alice@example.com");

    javelin::jmap::cache::QueryService queryService{databaseContext.connection};
    const auto cachedWindowResult = queryService.loadSearchWindow("u1", "session-query", 0, 100);
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
    CHECK_FALSE(cachedThread.has_value());

    const auto childResult = emailRepository.find("u1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(childResult));
    CHECK_FALSE(std::get<std::optional<javelin::jmap::domain::Email>>(childResult).has_value());
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
    CHECK((*inboxPage)->coverage == javelin::jmap::cache::QueryWindowCoverage::LocallyProjected);
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

TEST_CASE("JmapCore rejects an invalid email mutation group atomically",
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

    javelin::jmap::cache::EmailRepository emails{databaseContext.connection};
    REQUIRE_FALSE(emails.replaceAll("account-1", {email}).has_value());
    FakeTransport transport;
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};

    const auto result =
        core.queueEmailMailboxMutations("account-1", {
                                                         {
                                                             .emailId = "eml-1",
                                                             .addMailboxIds = {"mbx-archive"},
                                                             .removeMailboxIds = {"mbx-inbox"},
                                                             .operationGroupId = "atomic-group",
                                                         },
                                                         {
                                                             .emailId = "missing",
                                                             .addKeywords = {"$flagged"},
                                                             .operationGroupId = "atomic-group",
                                                         },
                                                     });
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));

    const auto unchanged = emails.find("account-1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(unchanged));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Email>>(unchanged).has_value());
    CHECK(std::get<std::optional<javelin::jmap::domain::Email>>(unchanged)->mailboxIds ==
          std::vector<std::string>{"mbx-inbox"});

    javelin::jmap::sync::EmailMutationJournal journal{databaseContext.connection};
    const auto grouped = journal.listForOperationGroup("account-1", "atomic-group");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::EmailMutationRecord>>(grouped));
    CHECK(std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(grouped).empty());
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
                         .addKeywords = {},
                         .removeKeywords = {},
                         .operationGroupId = "history-group-1",
                         .ifInState = "email-state-1",
                         .authoritativeMailboxIds = std::nullopt,
                         .authoritativeKeywords = std::nullopt,
                     });
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(moveResult));
    const auto& queued = std::get<javelin::jmap::QueuedEmailMutation>(moveResult);
    CHECK(queued.patch.addMailboxIds == std::vector<std::string>{"mbx-archive"});
    CHECK(queued.patch.removeMailboxIds == std::vector<std::string>{"mbx-inbox", "mbx-projects"});
    CHECK(queued.patch.operationGroupId == std::optional<std::string>{"history-group-1"});
    CHECK(queued.patch.ifInState == std::optional<std::string>{"email-state-1"});

    javelin::jmap::sync::EmailMutationJournal journal{databaseContext.connection};
    const auto grouped = journal.listForOperationGroup("account-1", "history-group-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::EmailMutationRecord>>(grouped));
    const auto& groupedRecords =
        std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(grouped);
    REQUIRE(groupedRecords.size() == 1);
    CHECK(groupedRecords.front().mutationId == queued.mutationId);
    CHECK(groupedRecords.front().baseState == std::optional<std::string>{"email-state-1"});

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
        CHECK((*cached)->coverage == javelin::jmap::cache::QueryWindowCoverage::LocallyProjected);
    }
    const auto searchWindow = searchWindows.find("account-1", "search-key", 0, 100);
    const auto* cachedSearch =
        std::get_if<std::optional<javelin::jmap::cache::SearchWindowRecord>>(&searchWindow);
    REQUIRE(cachedSearch != nullptr);
    REQUIRE(cachedSearch->has_value());
    CHECK((*cachedSearch)->coverage == javelin::jmap::cache::QueryWindowCoverage::LocallyProjected);
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
    const std::string searchKey = "search:openrouter";
    javelin::jmap::cache::SearchWindowRepository searchWindows{databaseContext.connection};
    REQUIRE_FALSE(searchWindows
                      .replace({
                          .accountId = "account-1",
                          .queryKey = searchKey,
                          .offset = 0,
                          .limit = 100,
                          .position = 0,
                          .returnedLimit = 100,
                          .total = 1,
                          .queryState = "search-state-1",
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

    const auto searchPageResult = queryService.loadSearchWindow("account-1", searchKey, 0, 100);
    const auto* searchPage =
        std::get_if<std::optional<javelin::jmap::cache::SearchWindowPage>>(&searchPageResult);
    REQUIRE(searchPage != nullptr);
    REQUIRE(searchPage->has_value());
    CHECK((*searchPage)->coverage == javelin::jmap::cache::QueryWindowCoverage::LocallyProjected);
    REQUIRE((*searchPage)->items.size() == 1);
    CHECK_FALSE((*searchPage)->items.front().isUnread);

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
    javelin::jmap::cache::SyncStateRepository syncStates{databaseContext.connection};
    REQUIRE_FALSE(
        syncStates
            .upsert({.accountId = "u1", .objectType = "Email", .queryKey = {}}, "email-state-0")
            .has_value());

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
    const auto staleState =
        syncStates.find({.accountId = "u1", .objectType = "Email", .queryKey = {}});
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(staleState));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(staleState).has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(staleState)->stateToken ==
          "email-state-0");

    javelin::jmap::sync::EmailMutationJournal emailMutationJournal{databaseContext.connection};
    const auto pendingResult = emailMutationJournal.listForEmail("u1", "eml-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::EmailMutationRecord>>(
        pendingResult));
    CHECK(std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(pendingResult).empty());
}

TEST_CASE("JmapCore submits authoritative keyword mutations for uncached server messages",
          "[jmap][core][mutation-journal][tags]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessionRepository{databaseContext.connection};
    REQUIRE_FALSE(sessionRepository.replace("u1", loadSessionFixture()).has_value());

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            R"({"methodResponses":[["Email/set",{"accountId":"u1","oldState":"email-state-1","newState":"email-state-2","updated":{"server-only":null},"notUpdated":{}},"queued-email-set"]],"createdIds":{},"sessionState":"session-state-2"})",
    });

    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto queued = core.queueEmailMailboxMutation(
        "u1", {
                  .emailId = "server-only",
                  .removeKeywords = {"project-x"},
                  .operationGroupId = "tag-delete:test",
                  .authoritativeMailboxIds = std::vector<std::string>{"mbx-inbox"},
                  .authoritativeKeywords = std::vector<std::string>{"$seen", "project-x"},
              });
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(queued));

    const auto submitted = QCoro::waitFor(core.submitPendingEmailMutations(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1", "tag-delete:test"));
    REQUIRE(std::holds_alternative<javelin::jmap::SubmittedEmailMutations>(submitted));
    const auto& summary = std::get<javelin::jmap::SubmittedEmailMutations>(submitted);
    CHECK(summary.attemptedEmailCount == 1);
    CHECK(summary.updatedEmailCount == 1);
    CHECK(summary.failedEmailCount == 0);

    REQUIRE(transport.requests.size() == 1);
    CHECK(transport.requests.front().body.contains("\"keywords/project-x\":null"));
    CHECK_FALSE(transport.requests.front().body.contains("\"keywords\":{"));

    javelin::jmap::cache::EmailRepository emails{databaseContext.connection};
    const auto cached = emails.find("u1", "server-only");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(cached));
    CHECK_FALSE(std::get<std::optional<javelin::jmap::domain::Email>>(cached).has_value());

    javelin::jmap::sync::EmailMutationJournal journal{databaseContext.connection};
    const auto records = journal.listForOperationGroup("u1", "tag-delete:test");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::EmailMutationRecord>>(records));
    CHECK(std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(records).empty());
}

TEST_CASE("JmapCore rejects authoritative keyword mutations without fabricating uncached messages",
          "[jmap][core][mutation-journal][tags]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessionRepository{databaseContext.connection};
    REQUIRE_FALSE(sessionRepository.replace("u1", loadSessionFixture()).has_value());

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            R"({"methodResponses":[["Email/set",{"accountId":"u1","oldState":"email-state-1","newState":"email-state-1","updated":{},"notUpdated":{"server-only":{"type":"forbidden"}}},"queued-email-set"]],"createdIds":{},"sessionState":"session-state-2"})",
    });

    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto queued = core.queueEmailMailboxMutation(
        "u1", {
                  .emailId = "server-only",
                  .removeKeywords = {"project-x"},
                  .operationGroupId = "tag-delete:test",
                  .authoritativeMailboxIds = std::vector<std::string>{"mbx-inbox"},
                  .authoritativeKeywords = std::vector<std::string>{"$seen", "project-x"},
              });
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(queued));

    const auto submitted = QCoro::waitFor(core.submitPendingEmailMutations(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1", "tag-delete:test"));
    REQUIRE(std::holds_alternative<javelin::jmap::SubmittedEmailMutations>(submitted));
    CHECK(std::get<javelin::jmap::SubmittedEmailMutations>(submitted).failedEmailCount == 1);

    javelin::jmap::cache::EmailRepository emails{databaseContext.connection};
    const auto cached = emails.find("u1", "server-only");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(cached));
    CHECK_FALSE(std::get<std::optional<javelin::jmap::domain::Email>>(cached).has_value());

    javelin::jmap::sync::EmailMutationJournal journal{databaseContext.connection};
    const auto records = journal.listForOperationGroup("u1", "tag-delete:test");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::EmailMutationRecord>>(records));
    const auto& mutations =
        std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(records);
    REQUIRE(mutations.size() == 1);
    CHECK(mutations.front().status == javelin::jmap::sync::MutationStatus::Rejected);
}

TEST_CASE("JmapCore preserves ambiguous authoritative keyword mutations without caching messages",
          "[jmap][core][mutation-journal][tags]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessionRepository{databaseContext.connection};
    REQUIRE_FALSE(sessionRepository.replace("u1", loadSessionFixture()).has_value());

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
        .message = "Connection closed after request dispatch",
    });

    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto queued = core.queueEmailMailboxMutation(
        "u1", {
                  .emailId = "server-only",
                  .removeKeywords = {"project-x"},
                  .operationGroupId = "tag-delete:test",
                  .authoritativeMailboxIds = std::vector<std::string>{"mbx-inbox"},
                  .authoritativeKeywords = std::vector<std::string>{"$seen", "project-x"},
              });
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(queued));

    const auto submitted = QCoro::waitFor(core.submitPendingEmailMutations(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1", "tag-delete:test"));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(submitted));

    javelin::jmap::cache::EmailRepository emails{databaseContext.connection};
    const auto cached = emails.find("u1", "server-only");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(cached));
    CHECK_FALSE(std::get<std::optional<javelin::jmap::domain::Email>>(cached).has_value());

    javelin::jmap::sync::EmailMutationJournal journal{databaseContext.connection};
    const auto records = journal.listForOperationGroup("u1", "tag-delete:test");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::EmailMutationRecord>>(records));
    const auto& mutations =
        std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(records);
    REQUIRE(mutations.size() == 1);
    CHECK(mutations.front().status == javelin::jmap::sync::MutationStatus::Unknown);
    CHECK(mutations.front().baseMailboxIds == std::vector<std::string>{"mbx-inbox"});
    CHECK(mutations.front().baseKeywords == std::vector<std::string>{"$seen", "project-x"});
}

TEST_CASE("JmapCore keeps newer optimistic mutations projected while an older submit completes",
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
    javelin::jmap::cache::SyncStateRepository syncStates{databaseContext.connection};
    REQUIRE_FALSE(
        syncStates
            .upsert({.accountId = "u1", .objectType = "Email", .queryKey = {}}, "email-state-1")
            .has_value());

    PendingTransport transport;
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto firstQueued =
        core.queueEmailMailboxMutation("u1", {
                                                 .emailId = "eml-1",
                                                 .addMailboxIds = {"mbx-archive"},
                                                 .removeMailboxIds = {"mbx-inbox"},
                                                 .operationGroupId = "group-1",
                                             });
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(firstQueued));

    std::optional<javelin::jmap::SubmittedEmailMutationsResult> submitted;
    QEventLoop completionLoop;
    auto submission = core.submitPendingEmailMutations(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1", "group-1");
    QCoro::connect(std::move(submission), QCoreApplication::instance(),
                   [&](javelin::jmap::SubmittedEmailMutationsResult result)
                   {
                       submitted = std::move(result);
                       completionLoop.quit();
                   });
    REQUIRE(transport.started);

    const auto secondQueued =
        core.queueEmailMailboxMutation("u1", {
                                                 .emailId = "eml-1",
                                                 .addMailboxIds = {"mbx-inbox"},
                                                 .removeMailboxIds = {"mbx-archive"},
                                                 .operationGroupId = "group-2",
                                             });
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(secondQueued));

    const auto optimistic = emailRepository.find("u1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(optimistic));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Email>>(optimistic).has_value());
    CHECK(std::get<std::optional<javelin::jmap::domain::Email>>(optimistic)->mailboxIds ==
          std::vector<std::string>{"mbx-inbox"});

    transport.complete(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            R"({"methodResponses":[["Email/set",{"accountId":"u1","oldState":"email-state-1","newState":"email-state-2","updated":{"eml-1":null},"notUpdated":{}},"queued-email-set"]],"createdIds":{},"sessionState":"session-state-2"})",
    });
    if (!submitted.has_value())
        completionLoop.exec();

    REQUIRE(submitted.has_value());
    REQUIRE(std::holds_alternative<javelin::jmap::SubmittedEmailMutations>(*submitted));
    CHECK(std::get<javelin::jmap::SubmittedEmailMutations>(*submitted).updatedEmailCount == 1);

    const auto retained = emailRepository.find("u1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(retained));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Email>>(retained).has_value());
    CHECK(std::get<std::optional<javelin::jmap::domain::Email>>(retained)->mailboxIds ==
          std::vector<std::string>{"mbx-inbox"});

    javelin::jmap::sync::EmailMutationJournal journal{databaseContext.connection};
    const auto remaining = journal.listForEmail("u1", "eml-1");
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::sync::EmailMutationRecord>>(remaining));
    const auto& records =
        std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(remaining);
    REQUIRE(records.size() == 2);
    CHECK(records[0].operationGroupId == std::optional<std::string>{"group-1"});
    CHECK(records[0].status == javelin::jmap::sync::MutationStatus::Accepted);
    CHECK(records[1].operationGroupId == std::optional<std::string>{"group-2"});
    CHECK(records[1].status == javelin::jmap::sync::MutationStatus::Pending);

    FakeTransport rejectionTransport;
    rejectionTransport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            R"({"methodResponses":[["Email/set",{"accountId":"u1","oldState":"email-state-2","newState":"email-state-2","updated":{},"notUpdated":{"eml-1":{"type":"forbidden"}}},"queued-email-set"]],"createdIds":{},"sessionState":"session-state-3"})",
    });
    javelin::jmap::JmapCore rejectionCore{databaseContext.connection, rejectionTransport,
                                          rejectionTransport.methodTransport};
    const auto rejected = QCoro::waitFor(rejectionCore.submitPendingEmailMutations(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1", "group-2"));
    REQUIRE(std::holds_alternative<javelin::jmap::SubmittedEmailMutations>(rejected));
    CHECK(std::get<javelin::jmap::SubmittedEmailMutations>(rejected).failedEmailCount == 1);

    const auto confirmed = emailRepository.find("u1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(confirmed));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Email>>(confirmed).has_value());
    CHECK(std::get<std::optional<javelin::jmap::domain::Email>>(confirmed)->mailboxIds ==
          std::vector<std::string>{"mbx-archive"});

    const auto settled = journal.listForEmail("u1", "eml-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::EmailMutationRecord>>(settled));
    const auto& settledRecords =
        std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(settled);
    REQUIRE(settledRecords.size() == 1);
    CHECK(settledRecords.front().operationGroupId == std::optional<std::string>{"group-2"});
    CHECK(settledRecords.front().status == javelin::jmap::sync::MutationStatus::Rejected);
}

TEST_CASE("JmapCore keeps newer optimistic mutations projected when an older submit is rejected",
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
    javelin::jmap::cache::SyncStateRepository syncStates{databaseContext.connection};
    REQUIRE_FALSE(
        syncStates
            .upsert({.accountId = "u1", .objectType = "Email", .queryKey = {}}, "email-state-1")
            .has_value());

    PendingTransport transport;
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto firstQueued =
        core.queueEmailMailboxMutation("u1", {
                                                 .emailId = "eml-1",
                                                 .addMailboxIds = {"mbx-archive"},
                                                 .removeMailboxIds = {"mbx-inbox"},
                                                 .operationGroupId = "group-1",
                                             });
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(firstQueued));

    std::optional<javelin::jmap::SubmittedEmailMutationsResult> submitted;
    QEventLoop completionLoop;
    auto submission = core.submitPendingEmailMutations(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1", "group-1");
    QCoro::connect(std::move(submission), QCoreApplication::instance(),
                   [&](javelin::jmap::SubmittedEmailMutationsResult result)
                   {
                       submitted = std::move(result);
                       completionLoop.quit();
                   });
    REQUIRE(transport.started);

    const auto secondQueued =
        core.queueEmailMailboxMutation("u1", {
                                                 .emailId = "eml-1",
                                                 .addKeywords = {"$flagged"},
                                                 .operationGroupId = "group-2",
                                             });
    REQUIRE(std::holds_alternative<javelin::jmap::QueuedEmailMutation>(secondQueued));

    transport.complete(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            R"({"methodResponses":[["Email/set",{"accountId":"u1","oldState":"email-state-1","newState":"email-state-1","updated":{},"notUpdated":{"eml-1":{"type":"forbidden"}}},"queued-email-set"]],"createdIds":{},"sessionState":"session-state-2"})",
    });
    if (!submitted.has_value())
        completionLoop.exec();

    REQUIRE(submitted.has_value());
    REQUIRE(std::holds_alternative<javelin::jmap::SubmittedEmailMutations>(*submitted));
    CHECK(std::get<javelin::jmap::SubmittedEmailMutations>(*submitted).failedEmailCount == 1);

    const auto retained = emailRepository.find("u1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(retained));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Email>>(retained).has_value());
    const auto& retainedEmail = *std::get<std::optional<javelin::jmap::domain::Email>>(retained);
    CHECK(retainedEmail.mailboxIds == std::vector<std::string>{"mbx-inbox"});
    CHECK(retainedEmail.keywords.size() == 2);
    CHECK(std::ranges::contains(retainedEmail.keywords, std::string{"$seen"}));
    CHECK(std::ranges::contains(retainedEmail.keywords, std::string{"$flagged"}));

    javelin::jmap::sync::EmailMutationJournal journal{databaseContext.connection};
    const auto remaining = journal.listForEmail("u1", "eml-1");
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::sync::EmailMutationRecord>>(remaining));
    const auto& records =
        std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(remaining);
    REQUIRE(records.size() == 2);
    CHECK(records[0].status == javelin::jmap::sync::MutationStatus::Rejected);
    CHECK(records[1].operationGroupId == std::optional<std::string>{"group-2"});
    CHECK(records[1].status == javelin::jmap::sync::MutationStatus::Pending);
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
        QStringLiteral("CREATE TRIGGER reject_email_acceptance BEFORE DELETE ON mutation_journal "
                       "BEGIN SELECT RAISE(ABORT,'acceptance rejected'); END")));

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
    javelin::jmap::cache::SyncStateRepository syncStates{databaseContext.connection};
    REQUIRE_FALSE(
        syncStates
            .upsert({.accountId = "u1", .objectType = "Email", .queryKey = {}}, "email-state-1")
            .has_value());

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
    const auto acceptedState =
        syncStates.find({.accountId = "u1", .objectType = "Email", .queryKey = {}});
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(
        acceptedState));
    REQUIRE(
        std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(acceptedState).has_value());
    CHECK(
        std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(acceptedState)->stateToken ==
        "email-state-2");
}

TEST_CASE("JmapCore hides a mailbox with optimistic Mailbox set semantics",
          "[jmap][core][mailbox][mutation-journal]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessions{databaseContext.connection};
    REQUIRE_FALSE(sessions.replace("u1", loadSessionFixture()).has_value());
    seedMailbox(databaseContext.connection);

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArrayLiteral(
            R"({"methodResponses":[["Mailbox/set",{"accountId":"u1","oldState":"mailbox-state-1","newState":"mailbox-state-2","updated":{"mbx-inbox":null},"notUpdated":{}},"mailbox-subscription-set"]],"createdIds":{},"sessionState":"session-state-2"})"),
    });
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    bool projected = false;
    const auto result = QCoro::waitFor(
        core.setMailboxSubscribed({.sessionUrl = "https://mail.example.com/.well-known/jmap",
                                   .loginEmail = "alice@example.com",
                                   .apiKey = "access-token"},
                                  "u1", "mbx-inbox", false, [&projected] { projected = true; }));

    REQUIRE(std::holds_alternative<javelin::jmap::MailboxSubscriptionChange>(result));
    CHECK(projected);
    REQUIRE(transport.requests.size() == 1);
    CHECK(transport.requests.front().body.contains(
        QByteArrayLiteral("\"ifInState\":\"mailbox-state-1\"")));
    CHECK(transport.requests.front().body.contains(QByteArrayLiteral("\"isSubscribed\":false")));
    javelin::jmap::cache::MailboxRepository mailboxes{databaseContext.connection};
    const auto mailbox = mailboxes.find("u1", "mbx-inbox");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Mailbox>>(mailbox));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Mailbox>>(mailbox).has_value());
    CHECK_FALSE(std::get<std::optional<javelin::jmap::domain::Mailbox>>(mailbox)->isSubscribed);
}

TEST_CASE("JmapCore restores mailbox visibility when the server rejects Hide",
          "[jmap][core][mailbox][mutation-journal]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessions{databaseContext.connection};
    REQUIRE_FALSE(sessions.replace("u1", loadSessionFixture()).has_value());
    seedMailbox(databaseContext.connection);

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArrayLiteral(
            R"({"methodResponses":[["Mailbox/set",{"accountId":"u1","oldState":"mailbox-state-1","newState":"mailbox-state-1","updated":{},"notUpdated":{"mbx-inbox":{"type":"forbidden"}}},"mailbox-subscription-set"]],"createdIds":{},"sessionState":"session-state-2"})"),
    });
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto result = QCoro::waitFor(
        core.setMailboxSubscribed({.sessionUrl = "https://mail.example.com/.well-known/jmap",
                                   .loginEmail = "alice@example.com",
                                   .apiKey = "access-token"},
                                  "u1", "mbx-inbox", false));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
    CHECK(std::get<javelin::jmap::OperationError>(result).code ==
          javelin::jmap::OperationErrorCode::PermissionDenied);

    javelin::jmap::cache::MailboxRepository mailboxes{databaseContext.connection};
    const auto mailbox = mailboxes.find("u1", "mbx-inbox");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Mailbox>>(mailbox));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Mailbox>>(mailbox).has_value());
    CHECK(std::get<std::optional<javelin::jmap::domain::Mailbox>>(mailbox)->isSubscribed);
}

TEST_CASE("JmapCore reconciles an ambiguous Hide before retrying Mailbox set",
          "[jmap][core][mailbox][mutation-journal][recovery]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessions{databaseContext.connection};
    REQUIRE_FALSE(sessions.replace("u1", loadSessionFixture()).has_value());
    seedMailbox(databaseContext.connection);

    FakeTransport transport;
    transport.invokeDispatched = true;
    transport.queuedResults.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
        .message = "Connection closed after request dispatch",
    });
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto ambiguous = QCoro::waitFor(
        core.setMailboxSubscribed({.sessionUrl = "https://mail.example.com/.well-known/jmap",
                                   .loginEmail = "alice@example.com",
                                   .apiKey = "access-token"},
                                  "u1", "mbx-inbox", false));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(ambiguous));

    javelin::jmap::cache::MailboxRepository mailboxes{databaseContext.connection};
    javelin::jmap::sync::MailboxMutationJournal journal{databaseContext.connection, mailboxes};
    const auto active = journal.listActive("u1");
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::sync::MailboxSubscriptionMutationRecord>>(
            active));
    const auto& records =
        std::get<std::vector<javelin::jmap::sync::MailboxSubscriptionMutationRecord>>(active);
    REQUIRE(records.size() == 1);
    CHECK(records.front().status == javelin::jmap::sync::MutationStatus::Unknown);

    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArrayLiteral(
            R"({"methodResponses":[["Mailbox/get",{"accountId":"u1","state":"mailbox-state-1","list":[{"id":"mbx-inbox","name":"Inbox","parentId":null,"role":"inbox","sortOrder":10,"totalEmails":125,"unreadEmails":7,"totalThreads":98,"unreadThreads":5,"isSubscribed":true,"myRights":{"mayReadItems":true,"mayAddItems":true,"mayRemoveItems":true,"maySetSeen":true,"maySetKeywords":true,"mayCreateChild":false,"mayRename":false,"mayDelete":false,"maySubmit":true}}],"notFound":[]},"mailbox-mutation-reconcile"]],"createdIds":{},"sessionState":"session-state-2"})"),
    });
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArrayLiteral(
            R"({"methodResponses":[["Mailbox/set",{"accountId":"u1","oldState":"mailbox-state-1","newState":"mailbox-state-2","updated":{"mbx-inbox":null},"notUpdated":{}},"mailbox-subscription-set"]],"createdIds":{},"sessionState":"session-state-3"})"),
    });
    const auto recovered = QCoro::waitFor(core.reconcileMailboxSubscription(
        {.sessionUrl = "https://mail.example.com/.well-known/jmap",
         .loginEmail = "alice@example.com",
         .apiKey = "access-token"},
        "u1"));
    REQUIRE(std::holds_alternative<javelin::jmap::MailboxSubscriptionChange>(recovered));
    CHECK(transport.requests.size() == 3);
    CHECK(transport.requests[1].body.contains(QByteArrayLiteral("Mailbox/get")));
    CHECK(transport.requests[2].body.contains(QByteArrayLiteral("Mailbox/set")));
    const auto settled = journal.listActive("u1");
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::sync::MailboxSubscriptionMutationRecord>>(
            settled));
    CHECK(std::get<std::vector<javelin::jmap::sync::MailboxSubscriptionMutationRecord>>(settled)
              .empty());
}

TEST_CASE("JmapCore refuses unsafe mailbox deletion before issuing Mailbox set",
          "[jmap][core][mailbox][destroy][safety]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    SECTION("server rights deny deletion")
    {
        auto databaseContext = makeDatabaseContext();
        javelin::jmap::cache::SessionRepository sessions{databaseContext.connection};
        REQUIRE_FALSE(sessions.replace("u1", loadSessionFixture()).has_value());
        seedDeletableMailbox(databaseContext.connection, 0, false);
        FakeTransport transport;
        javelin::jmap::JmapCore core{databaseContext.connection, transport,
                                     transport.methodTransport};
        const auto result = QCoro::waitFor(
            core.destroyMailbox({.sessionUrl = "https://mail.example.com/.well-known/jmap",
                                 .loginEmail = "alice@example.com",
                                 .apiKey = "access-token"},
                                "u1", "mbx-inbox"));
        REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
        CHECK(std::get<javelin::jmap::OperationError>(result).code ==
              javelin::jmap::OperationErrorCode::PermissionDenied);
        CHECK(transport.requests.empty());
    }

    SECTION("mailbox still contains messages")
    {
        auto databaseContext = makeDatabaseContext();
        javelin::jmap::cache::SessionRepository sessions{databaseContext.connection};
        REQUIRE_FALSE(sessions.replace("u1", loadSessionFixture()).has_value());
        seedDeletableMailbox(databaseContext.connection, 1, true);
        FakeTransport transport;
        javelin::jmap::JmapCore core{databaseContext.connection, transport,
                                     transport.methodTransport};
        const auto result = QCoro::waitFor(
            core.destroyMailbox({.sessionUrl = "https://mail.example.com/.well-known/jmap",
                                 .loginEmail = "alice@example.com",
                                 .apiKey = "access-token"},
                                "u1", "mbx-inbox"));
        REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
        CHECK(std::get<javelin::jmap::OperationError>(result).code ==
              javelin::jmap::OperationErrorCode::PreconditionFailed);
        CHECK(transport.requests.empty());
    }

    SECTION("mailbox has a child")
    {
        auto databaseContext = makeDatabaseContext();
        javelin::jmap::cache::SessionRepository sessions{databaseContext.connection};
        REQUIRE_FALSE(sessions.replace("u1", loadSessionFixture()).has_value());
        seedDeletableMailbox(databaseContext.connection);
        const auto parsed = javelin::jmap::domain::parseMailbox(
            javelin::tests::loadFixture("jmap/entities/mailbox.json"));
        REQUIRE(parsed.ok());
        REQUIRE(parsed.value.has_value());
        auto child = *parsed.value;
        child.id = "mbx-child";
        child.name = "Child";
        child.parentId = "mbx-inbox";
        child.role = std::nullopt;
        child.totalEmails = 0;
        child.unreadEmails = 0;
        child.totalThreads = 0;
        child.unreadThreads = 0;
        javelin::jmap::cache::MailboxRepository mailboxes{databaseContext.connection};
        REQUIRE_FALSE(mailboxes.upsertMany("u1", {child}).has_value());

        FakeTransport transport;
        javelin::jmap::JmapCore core{databaseContext.connection, transport,
                                     transport.methodTransport};
        const auto result = QCoro::waitFor(
            core.destroyMailbox({.sessionUrl = "https://mail.example.com/.well-known/jmap",
                                 .loginEmail = "alice@example.com",
                                 .apiKey = "access-token"},
                                "u1", "mbx-inbox"));
        REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
        CHECK(std::get<javelin::jmap::OperationError>(result).code ==
              javelin::jmap::OperationErrorCode::PreconditionFailed);
        CHECK(transport.requests.empty());
    }
}

TEST_CASE("JmapCore creates a top-level mailbox with an optimistic pending projection",
          "[jmap][core][mailbox][create][mutation-journal]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessions{databaseContext.connection};
    REQUIRE_FALSE(sessions.replace("u1", mailboxCreateSession()).has_value());
    seedMailbox(databaseContext.connection);

    FakeTransport transport;
    transport.responseFactory = [](const javelin::jmap::api::HttpRequest& request)
    {
        const auto creationId = mailboxCreationId(request);
        return javelin::jmap::api::TransportResult{javelin::jmap::api::HttpResponse{
            .statusCode = 200,
            .body = mailboxCreateSuccessEnvelope(creationId, "mailbox-state-1", "mailbox-state-2"),
        }};
    };
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    bool projected = false;
    const auto result = QCoro::waitFor(core.createMailbox(
        {.sessionUrl = "https://mail.example.com/.well-known/jmap",
         .loginEmail = "alice@example.com",
         .apiKey = "access-token"},
        "u1", "Projects",
        [&]
        {
            projected = true;
            QSqlQuery query{databaseContext.connection.database()};
            REQUIRE(query.exec(QStringLiteral(
                "SELECT COUNT(*) FROM mailbox_create_projections WHERE account_id='u1' AND "
                "name='Projects'")));
            REQUIRE(query.next());
            CHECK(query.value(0).toInt() == 1);
        }));

    REQUIRE(std::holds_alternative<javelin::jmap::MailboxCreateChange>(result));
    const auto& change = std::get<javelin::jmap::MailboxCreateChange>(result);
    CHECK(projected);
    CHECK(change.mailboxId == "mbx-projects");
    CHECK(change.name == "Projects");
    REQUIRE(transport.requests.size() == 1);
    CHECK(transport.requests.front().body.contains(QByteArrayLiteral("\"Mailbox/set\"")));
    CHECK(transport.requests.front().body.contains(QByteArrayLiteral("\"Mailbox/get\"")));
    CHECK(transport.requests.front().body.contains(QByteArrayLiteral("\"name\":\"Projects\"")));
    const auto requestCreationId = mailboxCreationId(transport.requests.front());
    CHECK(transport.requests.front().body.contains(
        QByteArray::fromStdString("/created/" + requestCreationId + "/id")));
    CHECK_FALSE(transport.requests.front().body.contains(QByteArrayLiteral("/created/*/id")));

    javelin::jmap::cache::MailboxRepository mailboxes{databaseContext.connection};
    const auto stored = mailboxes.find("u1", "mbx-projects");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Mailbox>>(stored));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Mailbox>>(stored).has_value());
    CHECK(std::get<std::optional<javelin::jmap::domain::Mailbox>>(stored)->name == "Projects");
    QSqlQuery projection{databaseContext.connection.database()};
    REQUIRE(projection.exec(
        QStringLiteral("SELECT COUNT(*) FROM mailbox_create_projections WHERE account_id='u1'")));
    REQUIRE(projection.next());
    CHECK(projection.value(0).toInt() == 0);
}

TEST_CASE("JmapCore validates top-level mailbox creation before projection",
          "[jmap][core][mailbox][create][permission]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    SECTION("server denies top-level creation")
    {
        auto databaseContext = makeDatabaseContext();
        javelin::jmap::cache::SessionRepository sessions{databaseContext.connection};
        REQUIRE_FALSE(sessions.replace("u1", loadSessionFixture()).has_value());
        seedMailbox(databaseContext.connection);
        FakeTransport transport;
        javelin::jmap::JmapCore core{databaseContext.connection, transport,
                                     transport.methodTransport};
        const auto result = QCoro::waitFor(
            core.createMailbox({.sessionUrl = "https://mail.example.com/.well-known/jmap",
                                .loginEmail = "alice@example.com",
                                .apiKey = "access-token"},
                               "u1", "Projects"));
        REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
        CHECK(std::get<javelin::jmap::OperationError>(result).code ==
              javelin::jmap::OperationErrorCode::PermissionDenied);
        CHECK(transport.requests.empty());
    }

    SECTION("duplicate sibling name")
    {
        auto databaseContext = makeDatabaseContext();
        javelin::jmap::cache::SessionRepository sessions{databaseContext.connection};
        REQUIRE_FALSE(sessions.replace("u1", mailboxCreateSession()).has_value());
        seedMailbox(databaseContext.connection);
        FakeTransport transport;
        javelin::jmap::JmapCore core{databaseContext.connection, transport,
                                     transport.methodTransport};
        const auto result = QCoro::waitFor(
            core.createMailbox({.sessionUrl = "https://mail.example.com/.well-known/jmap",
                                .loginEmail = "alice@example.com",
                                .apiKey = "access-token"},
                               "u1", "Inbox"));
        REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
        CHECK(std::get<javelin::jmap::OperationError>(result).code ==
              javelin::jmap::OperationErrorCode::InvalidUserInput);
        CHECK(transport.requests.empty());
    }
}

TEST_CASE("JmapCore removes a mailbox create projection after server rejection",
          "[jmap][core][mailbox][create][mutation-journal]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessions{databaseContext.connection};
    REQUIRE_FALSE(sessions.replace("u1", mailboxCreateSession()).has_value());
    seedMailbox(databaseContext.connection);

    FakeTransport transport;
    transport.responseFactory = [](const javelin::jmap::api::HttpRequest& request)
    {
        const auto creationId = mailboxCreationId(request);
        return javelin::jmap::api::TransportResult{javelin::jmap::api::HttpResponse{
            .statusCode = 200,
            .body =
                QStringLiteral(
                    R"({"methodResponses":[["Mailbox/set",{"accountId":"u1","oldState":"mailbox-state-1","newState":"mailbox-state-1","created":{},"notCreated":{"%1":{"type":"invalidProperties","description":"A sibling already has this name","properties":["name"]}}},"mailbox-create-set"],["error",{"type":"invalidResultReference"},"mailbox-create-get"]],"createdIds":{},"sessionState":"session-state-2"})")
                    .arg(QString::fromStdString(creationId))
                    .toUtf8(),
        }};
    };
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto result = QCoro::waitFor(
        core.createMailbox({.sessionUrl = "https://mail.example.com/.well-known/jmap",
                            .loginEmail = "alice@example.com",
                            .apiKey = "access-token"},
                           "u1", "Projects"));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
    CHECK(std::get<javelin::jmap::OperationError>(result).code ==
          javelin::jmap::OperationErrorCode::InvalidUserInput);
    QSqlQuery projection{databaseContext.connection.database()};
    REQUIRE(projection.exec(
        QStringLiteral("SELECT COUNT(*) FROM mailbox_create_projections WHERE account_id='u1'")));
    REQUIRE(projection.next());
    CHECK(projection.value(0).toInt() == 0);
}

TEST_CASE("JmapCore adopts an ambiguous mailbox creation already accepted by the server",
          "[jmap][core][mailbox][create][mutation-journal][recovery]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessions{databaseContext.connection};
    REQUIRE_FALSE(sessions.replace("u1", mailboxCreateSession()).has_value());
    seedMailbox(databaseContext.connection);

    FakeTransport transport;
    transport.invokeDispatched = true;
    transport.queuedResults.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
        .message = "Connection closed after request dispatch",
    });
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto ambiguous = QCoro::waitFor(
        core.createMailbox({.sessionUrl = "https://mail.example.com/.well-known/jmap",
                            .loginEmail = "alice@example.com",
                            .apiKey = "access-token"},
                           "u1", "Projects"));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(ambiguous));

    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArrayLiteral(
            R"({"methodResponses":[["Mailbox/get",{"accountId":"u1","state":"mailbox-state-2","list":[{"id":"mbx-projects","name":"Projects","parentId":null,"role":null,"sortOrder":0,"totalEmails":0,"unreadEmails":0,"totalThreads":0,"unreadThreads":0,"isSubscribed":true,"myRights":{"mayReadItems":true,"mayAddItems":true,"mayRemoveItems":true,"maySetSeen":true,"maySetKeywords":true,"mayCreateChild":true,"mayRename":true,"mayDelete":true,"maySubmit":true}}],"notFound":[]},"mailbox-create-reconcile"]],"createdIds":{},"sessionState":"session-state-2"})"),
    });
    const auto recovered = QCoro::waitFor(
        core.reconcileMailboxCreate({.sessionUrl = "https://mail.example.com/.well-known/jmap",
                                     .loginEmail = "alice@example.com",
                                     .apiKey = "access-token"},
                                    "u1"));
    REQUIRE(std::holds_alternative<javelin::jmap::MailboxCreateChange>(recovered));
    CHECK(std::get<javelin::jmap::MailboxCreateChange>(recovered).mailboxId == "mbx-projects");
    CHECK(transport.requests.size() == 2);

    javelin::jmap::cache::MailboxRepository mailboxes{databaseContext.connection};
    const auto stored = mailboxes.find("u1", "mbx-projects");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Mailbox>>(stored));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Mailbox>>(stored).has_value());
}

TEST_CASE("JmapCore safely retries an ambiguous mailbox creation proven absent",
          "[jmap][core][mailbox][create][mutation-journal][recovery]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessions{databaseContext.connection};
    REQUIRE_FALSE(sessions.replace("u1", mailboxCreateSession()).has_value());
    seedMailbox(databaseContext.connection);

    FakeTransport transport;
    transport.invokeDispatched = true;
    transport.queuedResults.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
        .message = "Connection closed after request dispatch",
    });
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto ambiguous = QCoro::waitFor(
        core.createMailbox({.sessionUrl = "https://mail.example.com/.well-known/jmap",
                            .loginEmail = "alice@example.com",
                            .apiKey = "access-token"},
                           "u1", "Projects"));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(ambiguous));

    QSqlQuery pending{databaseContext.connection.database()};
    REQUIRE(pending.exec(QStringLiteral(
        "SELECT creation_id FROM mailbox_create_projections WHERE account_id='u1'")));
    REQUIRE(pending.next());
    const auto creationId = pending.value(0).toString().toStdString();
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArrayLiteral(
            R"({"methodResponses":[["Mailbox/get",{"accountId":"u1","state":"mailbox-state-2","list":[],"notFound":[]},"mailbox-create-reconcile"]],"createdIds":{},"sessionState":"session-state-2"})"),
    });
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = mailboxCreateSuccessEnvelope(creationId, "mailbox-state-2", "mailbox-state-3"),
    });
    const auto recovered = QCoro::waitFor(
        core.reconcileMailboxCreate({.sessionUrl = "https://mail.example.com/.well-known/jmap",
                                     .loginEmail = "alice@example.com",
                                     .apiKey = "access-token"},
                                    "u1"));
    REQUIRE(std::holds_alternative<javelin::jmap::MailboxCreateChange>(recovered));
    CHECK(std::get<javelin::jmap::MailboxCreateChange>(recovered).mailboxId == "mbx-projects");
    REQUIRE(transport.requests.size() == 3);
    CHECK(transport.requests[2].body.contains(
        QByteArrayLiteral("\"ifInState\":\"mailbox-state-2\"")));
}

TEST_CASE("JmapCore deletes an empty mailbox with optimistic Mailbox set semantics",
          "[jmap][core][mailbox][destroy][mutation-journal]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessions{databaseContext.connection};
    REQUIRE_FALSE(sessions.replace("u1", loadSessionFixture()).has_value());
    seedDeletableMailbox(databaseContext.connection);

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArrayLiteral(
            R"({"methodResponses":[["Mailbox/set",{"accountId":"u1","oldState":"mailbox-state-1","newState":"mailbox-state-2","destroyed":["mbx-inbox"],"notDestroyed":{}},"mailbox-destroy-set"]],"createdIds":{},"sessionState":"session-state-2"})"),
    });
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    bool projected = false;
    const auto result = QCoro::waitFor(
        core.destroyMailbox({.sessionUrl = "https://mail.example.com/.well-known/jmap",
                             .loginEmail = "alice@example.com",
                             .apiKey = "access-token"},
                            "u1", "mbx-inbox", [&projected] { projected = true; }));

    REQUIRE(std::holds_alternative<javelin::jmap::MailboxDestroyChange>(result));
    CHECK(projected);
    REQUIRE(transport.requests.size() == 1);
    CHECK(transport.requests.front().body.contains(
        QByteArrayLiteral("\"ifInState\":\"mailbox-state-1\"")));
    CHECK(
        transport.requests.front().body.contains(QByteArrayLiteral("\"destroy\":[\"mbx-inbox\"]")));
    CHECK(transport.requests.front().body.contains(
        QByteArrayLiteral("\"onDestroyRemoveEmails\":false")));
    javelin::jmap::cache::MailboxRepository mailboxes{databaseContext.connection};
    const auto mailbox = mailboxes.find("u1", "mbx-inbox");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Mailbox>>(mailbox));
    CHECK_FALSE(std::get<std::optional<javelin::jmap::domain::Mailbox>>(mailbox).has_value());
}

TEST_CASE("JmapCore restores a mailbox when the server rejects deletion",
          "[jmap][core][mailbox][destroy][mutation-journal]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessions{databaseContext.connection};
    REQUIRE_FALSE(sessions.replace("u1", loadSessionFixture()).has_value());
    seedDeletableMailbox(databaseContext.connection);

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArrayLiteral(
            R"({"methodResponses":[["Mailbox/set",{"accountId":"u1","oldState":"mailbox-state-1","newState":"mailbox-state-1","destroyed":[],"notDestroyed":{"mbx-inbox":{"type":"mailboxHasEmail"}}},"mailbox-destroy-set"]],"createdIds":{},"sessionState":"session-state-2"})"),
    });
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto result = QCoro::waitFor(
        core.destroyMailbox({.sessionUrl = "https://mail.example.com/.well-known/jmap",
                             .loginEmail = "alice@example.com",
                             .apiKey = "access-token"},
                            "u1", "mbx-inbox"));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
    CHECK(std::get<javelin::jmap::OperationError>(result).code ==
          javelin::jmap::OperationErrorCode::PreconditionFailed);

    javelin::jmap::cache::MailboxRepository mailboxes{databaseContext.connection};
    const auto mailbox = mailboxes.find("u1", "mbx-inbox");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Mailbox>>(mailbox));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Mailbox>>(mailbox).has_value());
    CHECK(std::get<std::optional<javelin::jmap::domain::Mailbox>>(mailbox)->myRights.mayDelete);
}

TEST_CASE("JmapCore reconciles an ambiguous mailbox deletion before retrying",
          "[jmap][core][mailbox][destroy][mutation-journal][recovery]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessions{databaseContext.connection};
    REQUIRE_FALSE(sessions.replace("u1", loadSessionFixture()).has_value());
    seedDeletableMailbox(databaseContext.connection);

    FakeTransport transport;
    transport.invokeDispatched = true;
    transport.queuedResults.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
        .message = "Connection closed after request dispatch",
    });
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto ambiguous = QCoro::waitFor(
        core.destroyMailbox({.sessionUrl = "https://mail.example.com/.well-known/jmap",
                             .loginEmail = "alice@example.com",
                             .apiKey = "access-token"},
                            "u1", "mbx-inbox"));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(ambiguous));

    javelin::jmap::cache::MailboxRepository mailboxes{databaseContext.connection};
    javelin::jmap::sync::MailboxMutationJournal journal{databaseContext.connection, mailboxes};
    const auto active = journal.listActiveDestroys("u1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::MailboxDestroyMutationRecord>>(
        active));
    const auto& records =
        std::get<std::vector<javelin::jmap::sync::MailboxDestroyMutationRecord>>(active);
    REQUIRE(records.size() == 1);
    CHECK(records.front().status == javelin::jmap::sync::MutationStatus::Unknown);
    const auto projected = mailboxes.find("u1", "mbx-inbox");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Mailbox>>(projected));
    CHECK_FALSE(std::get<std::optional<javelin::jmap::domain::Mailbox>>(projected).has_value());

    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArrayLiteral(
            R"({"methodResponses":[["Mailbox/get",{"accountId":"u1","state":"mailbox-state-1","list":[{"id":"mbx-inbox","name":"Inbox","parentId":null,"role":"inbox","sortOrder":10,"totalEmails":0,"unreadEmails":0,"totalThreads":0,"unreadThreads":0,"isSubscribed":true,"myRights":{"mayReadItems":true,"mayAddItems":true,"mayRemoveItems":true,"maySetSeen":true,"maySetKeywords":true,"mayCreateChild":false,"mayRename":false,"mayDelete":true,"maySubmit":true}}],"notFound":[]},"mailbox-mutation-reconcile"]],"createdIds":{},"sessionState":"session-state-2"})"),
    });
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArrayLiteral(
            R"({"methodResponses":[["Mailbox/set",{"accountId":"u1","oldState":"mailbox-state-1","newState":"mailbox-state-2","destroyed":["mbx-inbox"],"notDestroyed":{}},"mailbox-destroy-set"]],"createdIds":{},"sessionState":"session-state-3"})"),
    });
    const auto recovered = QCoro::waitFor(
        core.reconcileMailboxDestroy({.sessionUrl = "https://mail.example.com/.well-known/jmap",
                                      .loginEmail = "alice@example.com",
                                      .apiKey = "access-token"},
                                     "u1"));
    REQUIRE(std::holds_alternative<javelin::jmap::MailboxDestroyChange>(recovered));
    CHECK(transport.requests.size() == 3);
    CHECK(transport.requests[1].body.contains(QByteArrayLiteral("Mailbox/get")));
    CHECK(transport.requests[2].body.contains(QByteArrayLiteral("Mailbox/set")));
    const auto settled = journal.listActiveDestroys("u1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::MailboxDestroyMutationRecord>>(
        settled));
    CHECK(
        std::get<std::vector<javelin::jmap::sync::MailboxDestroyMutationRecord>>(settled).empty());
}

TEST_CASE("JmapCore does not repeat an ambiguous deletion already accepted by the server",
          "[jmap][core][mailbox][destroy][mutation-journal][recovery]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessions{databaseContext.connection};
    REQUIRE_FALSE(sessions.replace("u1", loadSessionFixture()).has_value());
    seedDeletableMailbox(databaseContext.connection);

    FakeTransport transport;
    transport.invokeDispatched = true;
    transport.queuedResults.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
        .message = "Connection closed after request dispatch",
    });
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto ambiguous = QCoro::waitFor(
        core.destroyMailbox({.sessionUrl = "https://mail.example.com/.well-known/jmap",
                             .loginEmail = "alice@example.com",
                             .apiKey = "access-token"},
                            "u1", "mbx-inbox"));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(ambiguous));

    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArrayLiteral(
            R"({"methodResponses":[["Mailbox/get",{"accountId":"u1","state":"mailbox-state-2","list":[],"notFound":["mbx-inbox"]},"mailbox-mutation-reconcile"]],"createdIds":{},"sessionState":"session-state-2"})"),
    });
    const auto recovered = QCoro::waitFor(
        core.reconcileMailboxDestroy({.sessionUrl = "https://mail.example.com/.well-known/jmap",
                                      .loginEmail = "alice@example.com",
                                      .apiKey = "access-token"},
                                     "u1"));
    REQUIRE(std::holds_alternative<javelin::jmap::MailboxDestroyChange>(recovered));
    CHECK(transport.requests.size() == 2);
    CHECK(transport.requests[1].body.contains(QByteArrayLiteral("Mailbox/get")));

    javelin::jmap::cache::MailboxRepository mailboxes{databaseContext.connection};
    javelin::jmap::sync::MailboxMutationJournal journal{databaseContext.connection, mailboxes};
    const auto settled = journal.listActiveDestroys("u1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::MailboxDestroyMutationRecord>>(
        settled));
    CHECK(
        std::get<std::vector<javelin::jmap::sync::MailboxDestroyMutationRecord>>(settled).empty());
}

TEST_CASE("JmapCore restores an ambiguous deletion when the authoritative mailbox advanced",
          "[jmap][core][mailbox][destroy][mutation-journal][recovery]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessions{databaseContext.connection};
    REQUIRE_FALSE(sessions.replace("u1", loadSessionFixture()).has_value());
    seedDeletableMailbox(databaseContext.connection);

    FakeTransport transport;
    transport.invokeDispatched = true;
    transport.queuedResults.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
        .message = "Connection closed after request dispatch",
    });
    javelin::jmap::JmapCore core{databaseContext.connection, transport, transport.methodTransport};
    const auto ambiguous = QCoro::waitFor(
        core.destroyMailbox({.sessionUrl = "https://mail.example.com/.well-known/jmap",
                             .loginEmail = "alice@example.com",
                             .apiKey = "access-token"},
                            "u1", "mbx-inbox"));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(ambiguous));

    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArrayLiteral(
            R"({"methodResponses":[["Mailbox/get",{"accountId":"u1","state":"mailbox-state-2","list":[{"id":"mbx-inbox","name":"Inbox","parentId":null,"role":"inbox","sortOrder":10,"totalEmails":1,"unreadEmails":1,"totalThreads":1,"unreadThreads":1,"isSubscribed":true,"myRights":{"mayReadItems":true,"mayAddItems":true,"mayRemoveItems":true,"maySetSeen":true,"maySetKeywords":true,"mayCreateChild":false,"mayRename":false,"mayDelete":true,"maySubmit":true}}],"notFound":[]},"mailbox-mutation-reconcile"]],"createdIds":{},"sessionState":"session-state-2"})"),
    });
    const auto recovered = QCoro::waitFor(
        core.reconcileMailboxDestroy({.sessionUrl = "https://mail.example.com/.well-known/jmap",
                                      .loginEmail = "alice@example.com",
                                      .apiKey = "access-token"},
                                     "u1"));
    REQUIRE(std::holds_alternative<javelin::jmap::MailboxDestroyChange>(recovered));
    CHECK(transport.requests.size() == 2);

    javelin::jmap::cache::MailboxRepository mailboxes{databaseContext.connection};
    const auto restored = mailboxes.find("u1", "mbx-inbox");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Mailbox>>(restored));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Mailbox>>(restored).has_value());
    CHECK(std::get<std::optional<javelin::jmap::domain::Mailbox>>(restored)->totalEmails == 1);
    javelin::jmap::sync::MailboxMutationJournal journal{databaseContext.connection, mailboxes};
    const auto settled = journal.listActiveDestroys("u1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::MailboxDestroyMutationRecord>>(
        settled));
    CHECK(
        std::get<std::vector<javelin::jmap::sync::MailboxDestroyMutationRecord>>(settled).empty());
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
