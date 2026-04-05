#include "FixtureReader.h"
#include "jmap/JmapCore.h"
#include "jmap/api/SessionParser.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/InlinePartPayloadRepository.h"
#include "jmap/cache/MessageContentRepository.h"
#include "jmap/cache/SessionRepository.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUrlQuery>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
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

        [[nodiscard]] QCoro::Task<javelin::jmap::api::TransportResult>
        send(const javelin::jmap::api::HttpRequest& request) override
        {
            requests.push_back(request);
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
            .databasePath = context.temporaryDir.filePath("cache.sqlite3"),
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

    void seedEmail(javelin::jmap::cache::DatabaseConnection& connection)
    {
        QSqlQuery query{connection.database()};
        query.prepare("INSERT INTO emails ("
                      "account_id, email_id, thread_id, blob_id, received_at, subject, preview, "
                      "mailbox_ids_json, keywords_json, has_attachment, size"
                      ") VALUES ("
                      ":account_id, :email_id, :thread_id, :blob_id, :received_at, :subject, "
                      ":preview, :mailbox_ids_json, :keywords_json, :has_attachment, :size)");
        query.bindValue(":account_id", "u1");
        query.bindValue(":email_id", "eml-1");
        query.bindValue(":thread_id", "thr-1");
        query.bindValue(":blob_id", "blob-root");
        query.bindValue(":received_at", "2026-04-05T11:22:33Z");
        query.bindValue(":subject", "Inline image");
        query.bindValue(":preview", "Preview");
        query.bindValue(":mailbox_ids_json", "[]");
        query.bindValue(":keywords_json", "{}");
        query.bindValue(":has_attachment", 1);
        query.bindValue(":size", 512);
        REQUIRE(query.exec());
    }

} // namespace

TEST_CASE("JmapCore refreshMessageContent caches inline image payloads for HTML rendering",
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
        .body =
            R"({"methodResponses":[["Email/get",{"accountId":"u1","state":"email-state-1","list":[{"id":"eml-1","htmlBody":[{"partId":"2","blobId":"blob-html","size":32,"type":"text/html","charset":"utf-8"}],"attachments":[{"partId":"3","blobId":"blob-inline","size":7,"name":"chart.png","type":"image/png","disposition":"inline","cid":"chart@cid"}],"bodyValues":{"2":{"isEncodingProblem":false,"isTruncated":false,"value":"<img src=\"cid:chart@cid\">"}}}],"notFound":[]},"email-content"]],"createdIds":{},"sessionState":"session-state-2"})",
    });
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArrayLiteral("PNGDATA"),
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
    REQUIRE(transport.requests.size() == 2);
    CHECK(transport.requests.front().method == javelin::jmap::api::HttpMethod::Post);
    CHECK(transport.requests.back().method == javelin::jmap::api::HttpMethod::Get);
    CHECK(transport.requests.back().url.scheme() == QStringLiteral("https"));
    CHECK(transport.requests.back().url.host() == QStringLiteral("mail.example.com"));
    CHECK(transport.requests.back().url.path() ==
          QStringLiteral("/jmap/download/u1/blob-inline/chart.png"));
    const QUrlQuery downloadQuery{transport.requests.back().url};
    const auto downloadType = downloadQuery.queryItemValue(QStringLiteral("type"));
    CHECK_FALSE(downloadType.isEmpty());
    CHECK(downloadType.contains(QStringLiteral("image")));
    CHECK(transport.requests.back().headers.front().value == "Bearer access-token");

    javelin::jmap::cache::InlinePartPayloadRepository payloadRepository{databaseContext.connection};
    const auto payloadResult = payloadRepository.find("u1", "eml-1", "3");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::InlinePartPayload>>(
        payloadResult));
    const auto& payload =
        std::get<std::optional<javelin::jmap::cache::InlinePartPayload>>(payloadResult);
    REQUIRE(payload.has_value());
    CHECK(payload->blobId == "blob-inline");
    CHECK(payload->mediaType == "image/png");
    CHECK(payload->payload == QByteArrayLiteral("PNGDATA"));
}

TEST_CASE("JmapCore downloadAttachment fetches normal attachment payloads on demand",
          "[jmap][core][message-content]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessionRepository{databaseContext.connection};
    const auto session = loadSessionFixture();
    REQUIRE_FALSE(sessionRepository.replace("u1", session).has_value());
    seedEmail(databaseContext.connection);

    javelin::jmap::cache::MessageContentRepository contentRepository{databaseContext.connection};
    REQUIRE_FALSE(
        contentRepository
            .replaceForEmail("u1", "eml-1",
                             {
                                 javelin::jmap::cache::EmailPart{
                                     .emailId = "eml-1",
                                     .partId = "4",
                                     .parentPartId = std::nullopt,
                                     .blobId = std::optional<std::string>{"blob-pdf"},
                                     .kind = "attachment",
                                     .mediaType = "application/pdf",
                                     .name = std::optional<std::string>{"report.pdf"},
                                     .charset = std::nullopt,
                                     .disposition = std::optional<std::string>{"attachment"},
                                     .cid = std::nullopt,
                                     .size = 4096,
                                     .isInlineRenderable = false,
                                     .isBodySection = false,
                                 },
                             },
                             {})
            .has_value());

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArrayLiteral("%PDF-data"),
    });

    javelin::jmap::JmapCore core{databaseContext.connection, transport};
    const auto result = QCoro::waitFor(core.downloadAttachment(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1", "eml-1", "4"));

    if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
    {
        FAIL(error->message.toStdString());
    }

    REQUIRE(std::holds_alternative<javelin::jmap::AttachmentDownload>(result));
    const auto& download = std::get<javelin::jmap::AttachmentDownload>(result);
    CHECK(download.partId == "4");
    CHECK(download.name == std::optional<std::string>{"report.pdf"});
    CHECK(download.mediaType == "application/pdf");
    CHECK(download.payload == QByteArrayLiteral("%PDF-data"));
    CHECK_FALSE(download.usedCachedInlinePayload);

    REQUIRE(transport.requests.size() == 1);
    CHECK(transport.requests.front().method == javelin::jmap::api::HttpMethod::Get);
    CHECK(transport.requests.front().url.path() ==
          QStringLiteral("/jmap/download/u1/blob-pdf/report.pdf"));
    CHECK(transport.requests.front().headers.front().value == "Bearer access-token");
}

TEST_CASE("JmapCore downloadAttachment reuses cached inline payloads when available",
          "[jmap][core][message-content]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessionRepository{databaseContext.connection};
    const auto session = loadSessionFixture();
    REQUIRE_FALSE(sessionRepository.replace("u1", session).has_value());
    seedEmail(databaseContext.connection);

    javelin::jmap::cache::MessageContentRepository contentRepository{databaseContext.connection};
    REQUIRE_FALSE(contentRepository
                      .replaceForEmail("u1", "eml-1",
                                       {
                                           javelin::jmap::cache::EmailPart{
                                               .emailId = "eml-1",
                                               .partId = "3",
                                               .parentPartId = std::nullopt,
                                               .blobId = std::optional<std::string>{"blob-inline"},
                                               .kind = "attachment",
                                               .mediaType = "image/png",
                                               .name = std::optional<std::string>{"chart.png"},
                                               .charset = std::nullopt,
                                               .disposition = std::optional<std::string>{"inline"},
                                               .cid = std::optional<std::string>{"chart@cid"},
                                               .size = 7,
                                               .isInlineRenderable = true,
                                               .isBodySection = false,
                                           },
                                       },
                                       {})
                      .has_value());

    javelin::jmap::cache::InlinePartPayloadRepository payloadRepository{databaseContext.connection};
    REQUIRE_FALSE(payloadRepository
                      .upsert("u1",
                              {
                                  .emailId = "eml-1",
                                  .partId = "3",
                                  .blobId = "blob-inline",
                                  .mediaType = "image/png",
                                  .payload = QByteArrayLiteral("PNGDATA"),
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
        "u1", "eml-1", "3"));

    if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
    {
        FAIL(error->message.toStdString());
    }

    REQUIRE(std::holds_alternative<javelin::jmap::AttachmentDownload>(result));
    const auto& download = std::get<javelin::jmap::AttachmentDownload>(result);
    CHECK(download.partId == "3");
    CHECK(download.name == std::optional<std::string>{"chart.png"});
    CHECK(download.mediaType == "image/png");
    CHECK(download.payload == QByteArrayLiteral("PNGDATA"));
    CHECK(download.usedCachedInlinePayload);
    CHECK(transport.requests.empty());
}
