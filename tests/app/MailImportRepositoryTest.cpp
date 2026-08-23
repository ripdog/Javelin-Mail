#include "app/MailImportRepository.h"

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
            static char appName[] = "javelin-mail-import-repository-tests";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    [[nodiscard]] javelin::jmap::cache::DatabaseConnection openDatabase(const QString& path)
    {
        auto result = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = QStringLiteral("mail-import-test-%1")
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

TEST_CASE("mail import journal persists scan hierarchy and ambiguous item state",
          "[app][mail-import][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto database = openDatabase(directory.filePath(QStringLiteral("import.sqlite3")));
    seedAccount(database);
    MailImportRepository repository{database};

    const MailImportOperationRecord operation{
        .operationId = "import-1",
        .accountId = "account-1",
        .mailboxId = std::optional<std::string>{"archive"},
        .sourcePaths = {QStringLiteral("/tmp/backup")},
        .recreateHierarchy = true,
        .status = MailImportStatus::Preparing,
        .scanSealed = false,
        .title = QStringLiteral("Import backup"),
        .createdAt = QStringLiteral("2026-08-22T10:00:00.000Z"),
        .lastError = std::nullopt,
    };
    REQUIRE_FALSE(repository.createOperation(operation).has_value());

    const std::vector<MailImportMailboxRecord> mailboxes{
        {.ordinal = 0,
         .relativePath = QStringLiteral("Projects"),
         .parentRelativePath = std::nullopt,
         .displayName = QStringLiteral("Projects")},
        {.ordinal = 1,
         .relativePath = QStringLiteral("Projects/2026"),
         .parentRelativePath = QStringLiteral("Projects"),
         .displayName = QStringLiteral("2026")},
    };
    const MailImportSourceFingerprint fingerprint{
        .canonicalPath = QStringLiteral("/tmp/backup/Projects/2026/mail.mbox"),
        .size = 900,
        .lastModifiedMs = 123456,
    };
    const std::vector<MailImportItemRecord> items{
        {.itemId = "item-1",
         .ordinal = 0,
         .sourcePath = fingerprint.canonicalPath,
         .sourceRelativePath = QStringLiteral("Projects/2026/mail.mbox"),
         .sourceKind = MailImportFileKind::Mbox,
         .contentOffset = 10,
         .contentEnd = 410,
         .decodedSize = 350,
         .sourceFingerprint = fingerprint,
         .receivedAt = std::optional<std::string>{"2026-08-20T00:00:00Z"},
         .destinationRelativePath = QStringLiteral("Projects/2026")},
        {.itemId = "item-2",
         .ordinal = 1,
         .sourcePath = fingerprint.canonicalPath,
         .sourceRelativePath = QStringLiteral("Projects/2026/mail.mbox"),
         .sourceKind = MailImportFileKind::Mbox,
         .contentOffset = 410,
         .contentEnd = 900,
         .decodedSize = 420,
         .sourceFingerprint = fingerprint,
         .destinationRelativePath = QStringLiteral("Projects/2026")},
    };
    REQUIRE_FALSE(repository.replaceScan(operation.operationId, mailboxes, items).has_value());

    auto loaded = requireValue(repository.findOperation(operation.operationId));
    REQUIRE(loaded.has_value());
    CHECK(loaded->scanSealed);
    CHECK(loaded->status == MailImportStatus::Running);
    CHECK(loaded->sourcePaths == operation.sourcePaths);

    auto pendingMailbox = requireValue(repository.nextPendingMailbox(operation.operationId));
    REQUIRE(pendingMailbox.has_value());
    CHECK(pendingMailbox->relativePath == QStringLiteral("Projects"));
    REQUIRE_FALSE(repository
                      .resolveMailbox(operation.operationId, QStringLiteral("Projects"),
                                      MailImportMailboxPhase::Reused, "projects-id")
                      .has_value());
    REQUIRE_FALSE(repository
                      .resolveMailbox(operation.operationId, QStringLiteral("Projects/2026"),
                                      MailImportMailboxPhase::Created, "projects-2026-id")
                      .has_value());
    REQUIRE_FALSE(repository
                      .propagateMailboxResolution(operation.operationId,
                                                  QStringLiteral("Projects/2026"),
                                                  std::string_view{"projects-2026-id"})
                      .has_value());

    auto next = requireValue(repository.nextActionableItem(operation.operationId));
    REQUIRE(next.has_value());
    CHECK(next->itemId == "item-1");
    CHECK(next->resolvedMailboxId == std::optional<std::string>{"projects-2026-id"});

    REQUIRE_FALSE(
        repository
            .transitionItem("item-1", {.phase = MailImportItemPhase::Uploading,
                                       .sourceSha256 = std::optional<std::string>{"sha-1"}})
            .has_value());
    REQUIRE_FALSE(
        repository
            .transitionItem("item-1", {.phase = MailImportItemPhase::Uploaded,
                                       .sourceSha256 = std::optional<std::string>{"sha-1"},
                                       .uploadedBlobId = std::optional<std::string>{"blob-1"}})
            .has_value());
    REQUIRE_FALSE(
        repository
            .transitionItem("item-1", {.phase = MailImportItemPhase::Unknown,
                                       .sourceSha256 = std::optional<std::string>{"sha-1"},
                                       .uploadedBlobId = std::optional<std::string>{"blob-1"},
                                       .preState = std::optional<std::string>{"state-before"},
                                       .lastError = QStringLiteral("connection lost")})
            .has_value());

    const auto unknown = requireValue(repository.listUnknownItems(operation.operationId));
    REQUIRE(unknown.size() == 1);
    CHECK(unknown.front().itemId == "item-1");
    CHECK(unknown.front().preState == std::optional<std::string>{"state-before"});
    CHECK(unknown.front().uploadedBlobId == std::optional<std::string>{"blob-1"});

    REQUIRE_FALSE(repository
                      .transitionItem("item-2", {.phase = MailImportItemPhase::Reused,
                                                 .existingEmailId =
                                                     std::optional<std::string>{"existing-email"}})
                      .has_value());
    const auto progress = requireValue(repository.progress(operation.operationId));
    CHECK(progress.totalItems == 2);
    CHECK(progress.completedItems == 1);
    CHECK(progress.reusedItems == 1);
    CHECK(progress.unknownItems == 1);
    CHECK(progress.totalBytes == 770);
    CHECK(progress.completedBytes == 420);

    REQUIRE_FALSE(repository
                      .setStatus(operation.operationId, MailImportStatus::BlockedUnknown,
                                 QStringLiteral("Needs reconciliation"))
                      .has_value());
    const auto recoverable = requireValue(repository.listRecoverable());
    REQUIRE(recoverable.size() == 1);
    CHECK(recoverable.front().status == MailImportStatus::BlockedUnknown);
}

TEST_CASE("mail import subtree failure treats mailbox path characters literally",
          "[app][mail-import][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto database = openDatabase(directory.filePath(QStringLiteral("import.sqlite3")));
    seedAccount(database);
    MailImportRepository repository{database};

    const MailImportOperationRecord operation{
        .operationId = "import-hostile-paths",
        .accountId = "account-1",
        .mailboxId = std::nullopt,
        .sourcePaths = {QStringLiteral("/tmp/backup")},
        .recreateHierarchy = true,
        .status = MailImportStatus::Preparing,
        .scanSealed = false,
        .title = QStringLiteral("Import backup"),
        .createdAt = QStringLiteral("2026-08-22T10:00:00.000Z"),
        .lastError = std::nullopt,
    };
    REQUIRE_FALSE(repository.createOperation(operation).has_value());

    const MailImportSourceFingerprint fingerprint{
        .canonicalPath = QStringLiteral("/tmp/backup/messages.mbox"),
        .size = 100,
        .lastModifiedMs = 123456,
    };
    const std::vector<MailImportItemRecord> items{
        {.itemId = "literal-child",
         .ordinal = 0,
         .sourcePath = fingerprint.canonicalPath,
         .sourceKind = MailImportFileKind::Mbox,
         .decodedSize = 50,
         .sourceFingerprint = fingerprint,
         .destinationRelativePath = QStringLiteral("Sales_100%/Child")},
        {.itemId = "lookalike-child",
         .ordinal = 1,
         .sourcePath = fingerprint.canonicalPath,
         .sourceKind = MailImportFileKind::Mbox,
         .decodedSize = 50,
         .sourceFingerprint = fingerprint,
         .destinationRelativePath = QStringLiteral("SalesX100A/Child")},
    };
    REQUIRE_FALSE(repository.replaceScan(operation.operationId, {}, items).has_value());
    REQUIRE_FALSE(repository
                      .propagateMailboxResolution(operation.operationId,
                                                  QStringLiteral("Sales_100%"), std::nullopt,
                                                  QStringLiteral("cannot create mailbox"))
                      .has_value());

    QSqlQuery phases{database.database()};
    REQUIRE(phases.exec(
        QStringLiteral("SELECT item_id,phase FROM mail_import_items ORDER BY ordinal")));
    REQUIRE(phases.next());
    CHECK(phases.value(0).toString() == QStringLiteral("literal-child"));
    CHECK(phases.value(1).toString() == QStringLiteral("no_destination"));
    REQUIRE(phases.next());
    CHECK(phases.value(0).toString() == QStringLiteral("lookalike-child"));
    CHECK(phases.value(1).toString() == QStringLiteral("pending"));
}
