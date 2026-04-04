#include "jmap/cache/MessageContentRepository.h"

#include <QCoreApplication>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
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

    [[nodiscard]] QString makeConnectionName()
    {
        static int counter = 0;
        ++counter;
        return QStringLiteral("javelin-content-%1").arg(counter);
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

    void seedAccount(javelin::jmap::cache::DatabaseConnection& connection)
    {
        QSqlQuery query{connection.database()};
        query.prepare("INSERT INTO accounts (account_id, email_address, session_url, is_primary) "
                      "VALUES (:account_id, :email_address, :session_url, :is_primary)");
        query.bindValue(":account_id", "account-1");
        query.bindValue(":email_address", "alice@example.com");
        query.bindValue(":session_url", "https://mail.example.com/.well-known/jmap");
        query.bindValue(":is_primary", 1);
        REQUIRE(query.exec());
    }

    void seedEmail(javelin::jmap::cache::DatabaseConnection& connection)
    {
        QSqlQuery query{connection.database()};
        query.prepare("INSERT INTO emails (account_id, email_id, thread_id, blob_id, received_at, "
                      "subject, preview, "
                      "mailbox_ids_json, keywords_json, has_attachment, size) "
                      "VALUES (:account_id, :email_id, :thread_id, :blob_id, :received_at, "
                      ":subject, :preview, "
                      ":mailbox_ids_json, :keywords_json, :has_attachment, :size)");
        query.bindValue(":account_id", "account-1");
        query.bindValue(":email_id", "eml-1");
        query.bindValue(":thread_id", "thr-1");
        query.bindValue(":blob_id", "blob-root");
        query.bindValue(":received_at", "2026-04-05T11:22:33Z");
        query.bindValue(":subject", "Quarterly update");
        query.bindValue(":preview", "Here is the short preview text.");
        query.bindValue(":mailbox_ids_json", "[]");
        query.bindValue(":keywords_json", "{}");
        query.bindValue(":has_attachment", 1);
        query.bindValue(":size", 4096);
        REQUIRE(query.exec());
    }

} // namespace

TEST_CASE("message content repository round-trips MIME metadata and canonical body values",
          "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    seedEmail(databaseContext.connection);

    javelin::jmap::cache::MessageContentRepository repository{databaseContext.connection};
    const std::vector<javelin::jmap::cache::EmailPart> parts{
        {
            .emailId = "eml-1",
            .partId = "1",
            .parentPartId = std::nullopt,
            .blobId = std::nullopt,
            .kind = "multipart/alternative",
            .mediaType = "multipart/alternative",
            .name = std::nullopt,
            .charset = std::nullopt,
            .disposition = std::nullopt,
            .cid = std::nullopt,
            .size = 0,
            .isInlineRenderable = false,
            .isBodySection = false,
        },
        {
            .emailId = "eml-1",
            .partId = "1.2",
            .parentPartId = std::optional<std::string>{"1"},
            .blobId = std::optional<std::string>{"blob-html"},
            .kind = "body",
            .mediaType = "text/html",
            .name = std::nullopt,
            .charset = std::optional<std::string>{"utf-8"},
            .disposition = std::optional<std::string>{"inline"},
            .cid = std::nullopt,
            .size = 120,
            .isInlineRenderable = false,
            .isBodySection = true,
        },
        {
            .emailId = "eml-1",
            .partId = "2",
            .parentPartId = std::nullopt,
            .blobId = std::optional<std::string>{"blob-inline"},
            .kind = "attachment",
            .mediaType = "image/png",
            .name = std::optional<std::string>{"chart.png"},
            .charset = std::nullopt,
            .disposition = std::optional<std::string>{"inline"},
            .cid = std::optional<std::string>{"chart@cid"},
            .size = 2048,
            .isInlineRenderable = true,
            .isBodySection = false,
        },
    };
    const std::vector<javelin::jmap::cache::EmailBodyValue> bodyValues{
        {
            .emailId = "eml-1",
            .partId = "1.2",
            .blobId = std::optional<std::string>{"blob-html"},
            .isTruncated = false,
            .value = "<html><body><img src=\"cid:chart@cid\"></body></html>",
        },
    };

    REQUIRE_FALSE(repository.replaceForEmail("account-1", "eml-1", parts, bodyValues).has_value());

    const auto loadedParts = repository.loadParts("account-1", "eml-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::EmailPart>>(loadedParts));
    const auto& partRows = std::get<std::vector<javelin::jmap::cache::EmailPart>>(loadedParts);
    REQUIRE(partRows.size() == 3);
    CHECK(partRows.back().cid == std::optional<std::string>{"chart@cid"});
    CHECK(partRows.back().isInlineRenderable);
    CHECK(partRows.at(1).isBodySection);

    const auto loadedBodies = repository.loadBodyValues("account-1", "eml-1");
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::cache::EmailBodyValue>>(loadedBodies));
    const auto& bodyRows =
        std::get<std::vector<javelin::jmap::cache::EmailBodyValue>>(loadedBodies);
    REQUIRE(bodyRows.size() == 1);
    CHECK(bodyRows.front().partId == "1.2");
    CHECK(bodyRows.front().blobId == std::optional<std::string>{"blob-html"});
}

TEST_CASE("message content repository replacement removes stale MIME rows",
          "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    seedEmail(databaseContext.connection);

    javelin::jmap::cache::MessageContentRepository repository{databaseContext.connection};
    REQUIRE_FALSE(repository
                      .replaceForEmail("account-1", "eml-1",
                                       {javelin::jmap::cache::EmailPart{
                                           .emailId = "eml-1",
                                           .partId = "1",
                                           .parentPartId = std::nullopt,
                                           .blobId = std::nullopt,
                                           .kind = "body",
                                           .mediaType = "text/plain",
                                           .name = std::nullopt,
                                           .charset = std::optional<std::string>{"utf-8"},
                                           .disposition = std::nullopt,
                                           .cid = std::nullopt,
                                           .size = 20,
                                           .isInlineRenderable = false,
                                           .isBodySection = true,
                                       }},
                                       {javelin::jmap::cache::EmailBodyValue{
                                           .emailId = "eml-1",
                                           .partId = "1",
                                           .blobId = std::nullopt,
                                           .isTruncated = false,
                                           .value = "body one",
                                       }})
                      .has_value());

    REQUIRE_FALSE(repository.replaceForEmail("account-1", "eml-1", {}, {}).has_value());
    CHECK(std::get<std::vector<javelin::jmap::cache::EmailPart>>(
              repository.loadParts("account-1", "eml-1"))
              .empty());
    CHECK(std::get<std::vector<javelin::jmap::cache::EmailBodyValue>>(
              repository.loadBodyValues("account-1", "eml-1"))
              .empty());
}
