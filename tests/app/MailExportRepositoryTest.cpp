#include "app/MailExportRepository.h"

#include <QCoreApplication>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <variant>

namespace
{
    using namespace javelin::app;
    using DatabaseError = javelin::jmap::cache::DatabaseError;

    class ApplicationGuard
    {
      public:
        ApplicationGuard()
        {
            if (QCoreApplication::instance() != nullptr)
                return;
            static int argc = 1;
            static char appName[] = "javelin-mail-export-repository-tests";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    [[nodiscard]] javelin::jmap::cache::DatabaseConnection openDatabase(const QString& path)
    {
        auto result = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = QStringLiteral("mail-export-test-%1")
                                  .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)),
            .databasePath = path,
        });
        if (const auto* error = std::get_if<DatabaseError>(&result))
            FAIL(error->message.toStdString());
        return std::get<javelin::jmap::cache::DatabaseConnection>(std::move(result));
    }

    void seedAccount(javelin::jmap::cache::DatabaseConnection& database)
    {
        QSqlQuery query{database.database()};
        REQUIRE(query.exec(QStringLiteral(
            "INSERT INTO accounts(account_id,email_address,session_url,is_primary) VALUES "
            "('account-1','alice@example.test','https://example.test/jmap',1)")));
    }

    template <typename T> [[nodiscard]] T requireValue(std::variant<T, DatabaseError> result)
    {
        if (const auto* error = std::get_if<DatabaseError>(&result))
            FAIL(error->message.toStdString());
        return std::get<T>(std::move(result));
    }
} // namespace

TEST_CASE("mail export journal persists mailbox snapshot, progress, and recovery state",
          "[app][mail-export][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto database = openDatabase(directory.filePath(QStringLiteral("export.sqlite3")));
    seedAccount(database);
    MailExportRepository repository{database};

    const MailExportOperationRecord operation{
        .operationId = "export-1",
        .accountId = "account-1",
        .scopeKind = MailExportScopeKind::Account,
        .mailboxId = std::nullopt,
        .format = MailExportFormat::MboxRd,
        .destinationDirectory = directory.filePath(QStringLiteral("output")),
        .status = MailExportStatus::Preparing,
        .manifestSealed = false,
        .manifestEmailState = std::nullopt,
        .title = QStringLiteral("Export account"),
        .createdAt = QStringLiteral("2026-08-19T07:00:00.000+12:00"),
        .lastError = std::nullopt,
    };
    REQUIRE_FALSE(repository.createOperation(operation).has_value());

    const std::vector<MailExportMailboxRecord> mailboxes{
        {.ordinal = 0,
         .mailboxId = "inbox",
         .displayName = QStringLiteral("Inbox"),
         .relativePath = QStringLiteral("Inbox")},
        {.ordinal = 1,
         .mailboxId = "empty",
         .displayName = QStringLiteral("Empty"),
         .relativePath = QStringLiteral("Projects/Empty")},
    };
    REQUIRE_FALSE(repository.replaceMailboxes(operation.operationId, mailboxes).has_value());
    const auto loadedMailboxes = requireValue(repository.listMailboxes(operation.operationId));
    REQUIRE(loadedMailboxes.size() == 2);
    CHECK(loadedMailboxes[1].mailboxId == "empty");
    CHECK(loadedMailboxes[1].relativePath == QStringLiteral("Projects/Empty"));

    const std::vector<MailExportItemRecord> items{
        {.itemId = "item-1",
         .ordinal = 0,
         .mailboxId = "inbox",
         .emailId = "email-1",
         .blobId = "blob-1",
         .size = 100,
         .subject = std::optional<std::string>{"First"},
         .receivedAt = "2026-08-19T00:00:00Z",
         .senderName = std::optional<std::string>{"Alice"},
         .senderEmail = std::optional<std::string>{"alice@example.test"},
         .phase = MailExportItemPhase::Pending,
         .outputRelativePath = std::nullopt,
         .rawContentHash = std::nullopt,
         .mboxStartOffset = std::nullopt,
         .mboxEndOffset = std::nullopt,
         .lastError = std::nullopt},
        {.itemId = "item-2",
         .ordinal = 1,
         .mailboxId = "inbox",
         .emailId = "email-2",
         .blobId = "blob-2",
         .size = 250,
         .subject = std::nullopt,
         .receivedAt = "2026-08-19T00:01:00Z",
         .senderName = std::nullopt,
         .senderEmail = std::optional<std::string>{"bob@example.test"},
         .phase = MailExportItemPhase::Pending,
         .outputRelativePath = std::nullopt,
         .rawContentHash = std::nullopt,
         .mboxStartOffset = std::nullopt,
         .mboxEndOffset = std::nullopt,
         .lastError = std::nullopt},
    };
    REQUIRE_FALSE(repository.appendItems(operation.operationId, items).has_value());
    REQUIRE_FALSE(repository.sealManifest(operation.operationId, "email-state-7").has_value());

    auto loadedOperation = requireValue(repository.findOperation(operation.operationId));
    REQUIRE(loadedOperation.has_value());
    CHECK(loadedOperation->manifestSealed);
    CHECK(loadedOperation->manifestEmailState == std::optional<std::string>{"email-state-7"});
    CHECK(loadedOperation->status == MailExportStatus::Running);

    REQUIRE_FALSE(
        repository.markSourceReady("item-1", "hash-1", QStringLiteral("Inbox.mbox")).has_value());
    REQUIRE_FALSE(repository.markWriting("item-1", 0).has_value());
    auto writing = requireValue(repository.listWritingItems(operation.operationId));
    REQUIRE(writing.size() == 1);
    CHECK(writing.front().rawContentHash == std::optional<std::string>{"hash-1"});
    CHECK(writing.front().outputRelativePath ==
          std::optional<QString>{QStringLiteral("Inbox.mbox")});

    REQUIRE_FALSE(repository.markComplete("item-1", 123).has_value());
    const auto committedSize =
        requireValue(repository.committedMboxSize(operation.operationId, "inbox"));
    CHECK(committedSize == 123);
    REQUIRE_FALSE(
        repository.markFailed("item-2", QStringLiteral("source disappeared")).has_value());
    const auto progress = requireValue(repository.progress(operation.operationId));
    CHECK(progress.totalItems == 2);
    CHECK(progress.completedItems == 1);
    CHECK(progress.failedItems == 1);
    CHECK(progress.totalBytes == 350);
    CHECK(progress.completedBytes == 100);

    REQUIRE_FALSE(repository
                      .setStatus(operation.operationId, MailExportStatus::Partial,
                                 QStringLiteral("One message omitted"))
                      .has_value());
    loadedOperation = requireValue(repository.findOperation(operation.operationId));
    REQUIRE(loadedOperation.has_value());
    CHECK(loadedOperation->status == MailExportStatus::Partial);
    CHECK(loadedOperation->lastError ==
          std::optional<QString>{QStringLiteral("One message omitted")});
}
