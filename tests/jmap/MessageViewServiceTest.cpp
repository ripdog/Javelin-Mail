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
            .databasePath = context.temporaryDir.filePath(QStringLiteral("cache.sqlite3")),
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
        QSqlQuery emailQuery{connection.database()};
        emailQuery.prepare(QStringLiteral(
            "INSERT INTO emails ("
            "account_id, email_id, thread_id, blob_id, received_at, sent_at, subject, preview, "
            "mailbox_ids_json, keywords_json, has_attachment, size, state"
            ") VALUES ("
            ":account_id, :email_id, :thread_id, :blob_id, :received_at, :sent_at, :subject, "
            ":preview, :mailbox_ids_json, :keywords_json, :has_attachment, :size, :state)"));
        emailQuery.bindValue(QStringLiteral(":account_id"), QStringLiteral("account-1"));
        emailQuery.bindValue(QStringLiteral(":email_id"), QStringLiteral("eml-1"));
        emailQuery.bindValue(QStringLiteral(":thread_id"), QStringLiteral("thr-1"));
        emailQuery.bindValue(QStringLiteral(":blob_id"), QStringLiteral("blob-root"));
        emailQuery.bindValue(QStringLiteral(":received_at"),
                             QStringLiteral("2026-04-05T11:22:33Z"));
        emailQuery.bindValue(QStringLiteral(":sent_at"), QStringLiteral("2026-04-05T11:15:00Z"));
        emailQuery.bindValue(QStringLiteral(":subject"), QStringLiteral("Quarterly update"));
        emailQuery.bindValue(QStringLiteral(":preview"),
                             QStringLiteral("Here is the short preview text."));
        emailQuery.bindValue(QStringLiteral(":mailbox_ids_json"), QStringLiteral("[]"));
        emailQuery.bindValue(QStringLiteral(":keywords_json"), QStringLiteral("{}"));
        emailQuery.bindValue(QStringLiteral(":has_attachment"), 1);
        emailQuery.bindValue(QStringLiteral(":size"), 4096);
        emailQuery.bindValue(QStringLiteral(":state"), QVariant{});
        REQUIRE(emailQuery.exec());

        QSqlQuery mailboxQuery{connection.database()};
        mailboxQuery.prepare(
            QStringLiteral("INSERT INTO email_mailboxes (account_id, email_id, mailbox_id) "
                           "VALUES (:account_id, :email_id, :mailbox_id)"));
        mailboxQuery.bindValue(QStringLiteral(":account_id"), QStringLiteral("account-1"));
        mailboxQuery.bindValue(QStringLiteral(":email_id"), QStringLiteral("eml-1"));
        mailboxQuery.bindValue(QStringLiteral(":mailbox_id"), QStringLiteral("mbx-inbox"));
        REQUIRE(mailboxQuery.exec());

        QSqlQuery keywordQuery{connection.database()};
        keywordQuery.prepare(
            QStringLiteral("INSERT INTO email_keywords (account_id, email_id, keyword) "
                           "VALUES (:account_id, :email_id, :keyword)"));
        keywordQuery.bindValue(QStringLiteral(":account_id"), QStringLiteral("account-1"));
        keywordQuery.bindValue(QStringLiteral(":email_id"), QStringLiteral("eml-1"));
        keywordQuery.bindValue(QStringLiteral(":keyword"), QStringLiteral("$seen"));
        REQUIRE(keywordQuery.exec());

        QSqlQuery addressQuery{connection.database()};
        addressQuery.prepare(QStringLiteral(
            "INSERT INTO email_addresses ("
            "account_id, email_id, field_name, position, display_name, address"
            ") VALUES ("
            ":account_id, :email_id, :field_name, :position, :display_name, :address)"));
        addressQuery.bindValue(QStringLiteral(":account_id"), QStringLiteral("account-1"));
        addressQuery.bindValue(QStringLiteral(":email_id"), QStringLiteral("eml-1"));
        addressQuery.bindValue(QStringLiteral(":field_name"), QStringLiteral("from"));
        addressQuery.bindValue(QStringLiteral(":position"), 0);
        addressQuery.bindValue(QStringLiteral(":display_name"), QStringLiteral("Alice"));
        addressQuery.bindValue(QStringLiteral(":address"), QStringLiteral("alice@example.com"));
        REQUIRE(addressQuery.exec());
    }

    void seedMessageContent(javelin::jmap::cache::DatabaseConnection& connection,
                            const QByteArray& payload = QByteArrayLiteral(
                                "Subject: Quarterly update\r\n"
                                "Content-Type: multipart/related; boundary=\"b\"\r\n"
                                "\r\n"
                                "--b\r\n"
                                "Content-Type: text/plain; charset=utf-8\r\n"
                                "\r\n"
                                "Plain body\r\n"
                                "--b\r\n"
                                "Content-Type: text/html; charset=utf-8\r\n"
                                "\r\n"
                                "<p>HTML body</p>\r\n"
                                "--b\r\n"
                                "Content-Type: image/png; name=\"chart.png\"\r\n"
                                "Content-Disposition: inline; filename=\"chart.png\"\r\n"
                                "Content-ID: <chart@cid>\r\n"
                                "\r\n"
                                "PNGDATA\r\n"
                                "--b--\r\n"))
    {
        QSqlQuery query{connection.database()};
        query.prepare(QStringLiteral("INSERT INTO raw_message_sources ("
                                     "account_id, email_id, blob_id, payload"
                                     ") VALUES ("
                                     ":account_id, :email_id, :blob_id, :payload)"));
        query.bindValue(QStringLiteral(":account_id"), QStringLiteral("account-1"));
        query.bindValue(QStringLiteral(":email_id"), QStringLiteral("eml-1"));
        query.bindValue(QStringLiteral(":blob_id"), QStringLiteral("blob-root"));
        query.bindValue(QStringLiteral(":payload"), payload);
        REQUIRE(query.exec());
    }

} // namespace

TEST_CASE("message view service loads cached raw email bodies and attachments",
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
    CHECK_FALSE(snapshot->htmlBody->isTruncated);
    CHECK(snapshot->htmlBody->value == "<p>HTML body</p>");
    REQUIRE(snapshot->htmlRenderDocument.has_value());
    CHECK(snapshot->htmlRenderDocument->html.find("<!doctype html>") != std::string::npos);
    CHECK(snapshot->htmlRenderDocument->inlineResourceCount == 0);
    CHECK(snapshot->htmlRenderDocument->blockedRemoteResourceCount == 0);
    REQUIRE(snapshot->attachments.size() == 1);
    CHECK(snapshot->attachments.front().name == std::optional<std::string>{"chart.png"});
    CHECK(snapshot->attachments.front().cid == std::optional<std::string>{"chart@cid"});
    CHECK(snapshot->attachments.front().isInlineRenderable);
}

TEST_CASE("message view service supplies plain text for an email without a body",
          "[jmap][cache][message-view]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    seedEmail(databaseContext.connection);
    seedMessageContent(
        databaseContext.connection,
        QByteArrayLiteral("Subject: DMARC report\r\n"
                          "Content-Type: application/zip; name=\"report.zip\"\r\n"
                          "Content-Disposition: attachment; filename=\"report.zip\"\r\n"
                          "Content-Transfer-Encoding: base64\r\n"
                          "\r\n"
                          "UEsDBAoAAAAA\r\n"));

    javelin::jmap::cache::MessageViewService service{databaseContext.connection};
    const auto result = service.load("account-1", "eml-1");

    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::MessageViewSnapshot>>(result));
    const auto& snapshot =
        std::get<std::optional<javelin::jmap::cache::MessageViewSnapshot>>(result);
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->plainTextBody.has_value());
    CHECK(snapshot->plainTextBody->kind == javelin::jmap::cache::MessageBodyKind::PlainText);
    CHECK(snapshot->plainTextBody->value == "No content in email body.");
    CHECK_FALSE(snapshot->htmlBody.has_value());
    REQUIRE(snapshot->attachments.size() == 1);
    CHECK(snapshot->attachments.front().name == std::optional<std::string>{"report.zip"});
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
