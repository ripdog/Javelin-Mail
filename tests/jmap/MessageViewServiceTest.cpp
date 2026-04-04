#include "jmap/cache/MessageViewService.h"

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
        return QStringLiteral("javelin-message-view-%1").arg(counter);
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
        QSqlQuery emailQuery{connection.database()};
        emailQuery.prepare(
            "INSERT INTO emails ("
            "account_id, email_id, thread_id, blob_id, received_at, sent_at, subject, preview, "
            "mailbox_ids_json, keywords_json, has_attachment, size, state"
            ") VALUES ("
            ":account_id, :email_id, :thread_id, :blob_id, :received_at, :sent_at, :subject, "
            ":preview, :mailbox_ids_json, :keywords_json, :has_attachment, :size, :state)");
        emailQuery.bindValue(":account_id", "account-1");
        emailQuery.bindValue(":email_id", "eml-1");
        emailQuery.bindValue(":thread_id", "thr-1");
        emailQuery.bindValue(":blob_id", "blob-root");
        emailQuery.bindValue(":received_at", "2026-04-05T11:22:33Z");
        emailQuery.bindValue(":sent_at", "2026-04-05T11:15:00Z");
        emailQuery.bindValue(":subject", "Quarterly update");
        emailQuery.bindValue(":preview", "Here is the short preview text.");
        emailQuery.bindValue(":mailbox_ids_json", "[]");
        emailQuery.bindValue(":keywords_json", "{}");
        emailQuery.bindValue(":has_attachment", 1);
        emailQuery.bindValue(":size", 4096);
        emailQuery.bindValue(":state", QVariant{});
        REQUIRE(emailQuery.exec());

        QSqlQuery mailboxQuery{connection.database()};
        mailboxQuery.prepare("INSERT INTO email_mailboxes (account_id, email_id, mailbox_id) "
                             "VALUES (:account_id, :email_id, :mailbox_id)");
        mailboxQuery.bindValue(":account_id", "account-1");
        mailboxQuery.bindValue(":email_id", "eml-1");
        mailboxQuery.bindValue(":mailbox_id", "mbx-inbox");
        REQUIRE(mailboxQuery.exec());

        QSqlQuery keywordQuery{connection.database()};
        keywordQuery.prepare("INSERT INTO email_keywords (account_id, email_id, keyword) "
                             "VALUES (:account_id, :email_id, :keyword)");
        keywordQuery.bindValue(":account_id", "account-1");
        keywordQuery.bindValue(":email_id", "eml-1");
        keywordQuery.bindValue(":keyword", "$seen");
        REQUIRE(keywordQuery.exec());

        QSqlQuery addressQuery{connection.database()};
        addressQuery.prepare(
            "INSERT INTO email_addresses ("
            "account_id, email_id, field_name, position, display_name, address"
            ") VALUES ("
            ":account_id, :email_id, :field_name, :position, :display_name, :address)");
        addressQuery.bindValue(":account_id", "account-1");
        addressQuery.bindValue(":email_id", "eml-1");
        addressQuery.bindValue(":field_name", "from");
        addressQuery.bindValue(":position", 0);
        addressQuery.bindValue(":display_name", "Alice");
        addressQuery.bindValue(":address", "alice@example.com");
        REQUIRE(addressQuery.exec());
    }

    void seedMessageContent(javelin::jmap::cache::DatabaseConnection& connection)
    {
        QSqlQuery partQuery{connection.database()};
        partQuery.prepare(
            "INSERT INTO email_parts ("
            "account_id, email_id, part_id, parent_part_id, blob_id, kind, media_type, name, "
            "charset, disposition, cid, size, is_inline_renderable, is_body_section"
            ") VALUES ("
            ":account_id, :email_id, :part_id, :parent_part_id, :blob_id, :kind, :media_type, "
            ":name, :charset, :disposition, :cid, :size, :is_inline_renderable, :is_body_section)");

        auto insertPart = [&partQuery](const QString& partId, const QVariant& parentPartId,
                                       const QVariant& blobId, const QString& kind,
                                       const QString& mediaType, const QVariant& name,
                                       const QVariant& disposition, const QVariant& cid,
                                       const qulonglong size, const int isInlineRenderable,
                                       const int isBodySection)
        {
            partQuery.bindValue(":account_id", "account-1");
            partQuery.bindValue(":email_id", "eml-1");
            partQuery.bindValue(":part_id", partId);
            partQuery.bindValue(":parent_part_id", parentPartId);
            partQuery.bindValue(":blob_id", blobId);
            partQuery.bindValue(":kind", kind);
            partQuery.bindValue(":media_type", mediaType);
            partQuery.bindValue(":name", name);
            partQuery.bindValue(":charset", "utf-8");
            partQuery.bindValue(":disposition", disposition);
            partQuery.bindValue(":cid", cid);
            partQuery.bindValue(":size", size);
            partQuery.bindValue(":is_inline_renderable", isInlineRenderable);
            partQuery.bindValue(":is_body_section", isBodySection);
            REQUIRE(partQuery.exec());
        };

        insertPart("1", QVariant{}, QVariant{}, "body", "text/plain", QVariant{}, QVariant{},
                   QVariant{}, 44, 0, 1);
        insertPart("2", QVariant{}, "blob-html", "body", "text/html", QVariant{}, "inline",
                   QVariant{}, 88, 0, 1);
        insertPart("3", QVariant{}, "blob-inline", "attachment", "image/png", "chart.png", "inline",
                   "chart@cid", 2048, 1, 0);

        QSqlQuery bodyQuery{connection.database()};
        bodyQuery.prepare("INSERT INTO email_body_values ("
                          "account_id, email_id, part_id, blob_id, is_truncated, value"
                          ") VALUES ("
                          ":account_id, :email_id, :part_id, :blob_id, :is_truncated, :value)");

        auto insertBody = [&bodyQuery](const QString& partId, const QVariant& blobId,
                                       const bool isTruncated, const QString& value)
        {
            bodyQuery.bindValue(":account_id", "account-1");
            bodyQuery.bindValue(":email_id", "eml-1");
            bodyQuery.bindValue(":part_id", partId);
            bodyQuery.bindValue(":blob_id", blobId);
            bodyQuery.bindValue(":is_truncated", isTruncated ? 1 : 0);
            bodyQuery.bindValue(":value", value);
            REQUIRE(bodyQuery.exec());
        };

        insertBody("1", QVariant{}, false, "Plain body");
        insertBody("2", "blob-html", true, "<p>HTML body</p>");
    }

} // namespace

TEST_CASE("message view service loads cached email headers bodies and attachments",
          "[jmap][cache][message-view]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    seedEmail(databaseContext.connection);
    seedMessageContent(databaseContext.connection);

    javelin::jmap::cache::MessageViewService service{databaseContext.connection};
    const auto result = service.load("account-1", "eml-1");

    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::MessageViewSnapshot>>(result));
    const auto& snapshot =
        std::get<std::optional<javelin::jmap::cache::MessageViewSnapshot>>(result);
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->email.subject == std::optional<std::string>{"Quarterly update"});
    REQUIRE(snapshot->plainTextBody.has_value());
    CHECK(snapshot->plainTextBody->kind == javelin::jmap::cache::MessageBodyKind::PlainText);
    CHECK(snapshot->plainTextBody->value == "Plain body");
    REQUIRE(snapshot->htmlBody.has_value());
    CHECK(snapshot->htmlBody->kind == javelin::jmap::cache::MessageBodyKind::Html);
    CHECK(snapshot->htmlBody->isTruncated);
    CHECK(snapshot->htmlBody->value == "<p>HTML body</p>");
    REQUIRE(snapshot->attachments.size() == 1);
    CHECK(snapshot->attachments.front().name == std::optional<std::string>{"chart.png"});
    CHECK(snapshot->attachments.front().cid == std::optional<std::string>{"chart@cid"});
    CHECK(snapshot->attachments.front().isInlineRenderable);
}

TEST_CASE("message view service returns no value for missing emails", "[jmap][cache][message-view]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    javelin::jmap::cache::MessageViewService service{databaseContext.connection};
    const auto result = service.load("account-1", "missing");

    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::MessageViewSnapshot>>(result));
    CHECK_FALSE(
        std::get<std::optional<javelin::jmap::cache::MessageViewSnapshot>>(result).has_value());
}
