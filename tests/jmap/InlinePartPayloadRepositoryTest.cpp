#include "jmap/cache/InlinePartPayloadRepository.h"

#include <QCoreApplication>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <memory>
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
        return QStringLiteral("javelin-inline-payload-%1").arg(counter);
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

} // namespace

TEST_CASE("inline part payload repository round-trips cached inline blobs",
          "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    QSqlQuery seedAccount{databaseContext.connection.database()};
    seedAccount.prepare(
        QStringLiteral("INSERT INTO accounts (account_id, email_address, session_url, is_primary) "
                       "VALUES (:account_id, :email_address, :session_url, :is_primary)"));
    seedAccount.bindValue(QStringLiteral(":account_id"), QStringLiteral("account-1"));
    seedAccount.bindValue(QStringLiteral(":email_address"), QStringLiteral("alice@example.com"));
    seedAccount.bindValue(QStringLiteral(":session_url"),
                          QStringLiteral("https://mail.example.com/.well-known/jmap"));
    seedAccount.bindValue(QStringLiteral(":is_primary"), 1);
    REQUIRE(seedAccount.exec());

    javelin::jmap::cache::InlinePartPayloadRepository repository{databaseContext.connection};

    const javelin::jmap::cache::InlinePartPayload payload{
        .emailId = "eml-1",
        .partId = "3",
        .blobId = "blob-inline",
        .mediaType = "image/png",
        .payload = QByteArrayLiteral("PNGDATA"),
    };

    if (const auto error = repository.upsert("account-1", payload))
    {
        FAIL(error->message.toStdString());
    }
    const auto result = repository.find("account-1", "eml-1", "3");

    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::InlinePartPayload>>(result));
    const auto& loaded = std::get<std::optional<javelin::jmap::cache::InlinePartPayload>>(result);
    REQUIRE(loaded.has_value());
    CHECK(loaded->blobId == "blob-inline");
    CHECK(loaded->mediaType == "image/png");
    CHECK(loaded->payload == QByteArrayLiteral("PNGDATA"));
}
