#include "app/MailTransferRepository.h"
#include "jmap/cache/MailVault.h"

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
            static char appName[] = "javelin-mail-transfer-repository-tests";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    [[nodiscard]] javelin::jmap::cache::DatabaseConnection openDatabase(const QString& path)
    {
        auto result = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = QStringLiteral("mail-transfer-test-%1")
                                  .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)),
            .databasePath = path,
        });
        if (const auto* error = std::get_if<DatabaseError>(&result))
            FAIL(error->message.toStdString());
        return std::get<javelin::jmap::cache::DatabaseConnection>(std::move(result));
    }

    [[nodiscard]] MailTransferOperationRecord operation(std::string id = "transfer-1")
    {
        return {
            .operationId = std::move(id),
            .operationGroupId = std::optional<std::string>{"history-group-1"},
            .sourceAccountId = "source-local",
            .sourceMailboxId = std::optional<std::string>{"inbox"},
            .destinationAccountId = "destination-local",
            .destinationMailboxId = "archive",
            .operation = MailTransferOperation::Move,
            .topology = MailTransferTopology::CrossServerImport,
            .status = MailTransferStatus::Preparing,
            .title = QStringLiteral("Move 2 messages"),
            .lastError = std::nullopt,
            .historyEntryId = std::nullopt,
            .createdAt = {},
            .updatedAt = {},
        };
    }

    [[nodiscard]] MailTransferItemRecord item(std::string operationId, std::string itemId,
                                              const std::int64_t ordinal, std::string sourceEmailId)
    {
        return {
            .itemId = std::move(itemId),
            .operationId = std::move(operationId),
            .ordinal = ordinal,
            .sourceEmailId = std::move(sourceEmailId),
            .sourceBlobId = "source-blob-" + std::to_string(ordinal),
            .sourceEmailState = std::optional<std::string>{"email-state-7"},
            .sourceMailboxIds = {"inbox", "important"},
            .sourceKeywords = {"$seen", "$flagged", "custom-keyword"},
            .sourceMessageIds = {"message@example.test"},
            .sourceReceivedAt = std::optional<std::string>{"2026-08-15T08:00:00Z"},
            .sourceSize = static_cast<std::uint64_t>(1000 + ordinal),
            .sourceRemoveMailboxIds = {"inbox"},
            .sourceDestroy = false,
            .rawContentHash = std::nullopt,
            .destinationCreationId = "creation-" + std::to_string(ordinal),
            .destinationUploadBlobId = std::nullopt,
            .destinationPreState = std::nullopt,
            .destinationEmailId = std::nullopt,
            .destinationBlobId = std::nullopt,
            .destinationThreadId = std::nullopt,
            .destinationSize = std::nullopt,
            .reusedExisting = false,
            .destinationPriorMailboxIds = std::nullopt,
            .phase = MailTransferItemPhase::Prepared,
            .lastError = std::nullopt,
            .createdAt = {},
            .updatedAt = {},
        };
    }

    [[nodiscard]] javelin::jmap::cache::MailVaultObject
    seedVaultObject(javelin::jmap::cache::DatabaseConnection& database,
                    const QByteArray& payload = QByteArrayLiteral("transfer source"))
    {
        const auto vault = javelin::jmap::cache::MailVault::forDatabase(database);
        const auto installed = vault.install(payload);
        if (const auto* error = std::get_if<javelin::jmap::cache::MailVaultError>(&installed))
            FAIL(error->message.toStdString());
        const auto object = std::get<javelin::jmap::cache::MailVaultObject>(installed);
        QSqlQuery insert{database.database()};
        insert.prepare(QStringLiteral(
            "INSERT INTO mail_vault_objects(content_hash,relative_path,size) "
            "VALUES(:hash,:path,:size)"));
        insert.bindValue(QStringLiteral(":hash"), QString::fromStdString(object.contentHash));
        insert.bindValue(QStringLiteral(":path"), object.relativePath);
        insert.bindValue(QStringLiteral(":size"), static_cast<qulonglong>(object.size));
        REQUIRE(insert.exec());
        return object;
    }

    [[nodiscard]] int scalar(javelin::jmap::cache::DatabaseConnection& database,
                             const QString& statement)
    {
        QSqlQuery query{database.database()};
        REQUIRE(query.exec(statement));
        REQUIRE(query.next());
        return query.value(0).toInt();
    }

    [[nodiscard]] bool requireBool(std::variant<bool, DatabaseError> result)
    {
        if (const auto* error = std::get_if<DatabaseError>(&result))
            FAIL(error->message.toStdString());
        return std::get<bool>(result);
    }

    [[nodiscard]] MailTransferOperationRecord
    requireOperation(std::variant<std::optional<MailTransferOperationRecord>, DatabaseError> result)
    {
        if (const auto* error = std::get_if<DatabaseError>(&result))
            FAIL(error->message.toStdString());
        auto operation = std::get<std::optional<MailTransferOperationRecord>>(std::move(result));
        REQUIRE(operation.has_value());
        return std::move(*operation);
    }

    [[nodiscard]] std::vector<MailTransferItemRecord>
    requireItems(std::variant<std::vector<MailTransferItemRecord>, DatabaseError> result)
    {
        if (const auto* error = std::get_if<DatabaseError>(&result))
            FAIL(error->message.toStdString());
        return std::get<std::vector<MailTransferItemRecord>>(std::move(result));
    }
} // namespace

TEST_CASE("mail transfer journal persists exact source snapshots and item order",
          "[app][mail-transfer][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto database = openDatabase(directory.filePath(QStringLiteral("transfer.sqlite3")));
    MailTransferRepository repository{database};

    const auto transfer = operation();
    const std::vector items{
        item(transfer.operationId, "item-1", 0, "email-a"),
        item(transfer.operationId, "item-2", 1, "email-b"),
    };
    REQUIRE_FALSE(repository.create(transfer, items).has_value());

    const auto loaded = requireOperation(repository.findOperation(transfer.operationId));
    CHECK(loaded.operationId == transfer.operationId);
    CHECK(loaded.operationGroupId == transfer.operationGroupId);
    CHECK(loaded.sourceAccountId == "source-local");
    CHECK(loaded.sourceMailboxId == std::optional<std::string>{"inbox"});
    CHECK(loaded.destinationAccountId == "destination-local");
    CHECK(loaded.destinationMailboxId == "archive");
    CHECK(loaded.operation == MailTransferOperation::Move);
    CHECK(loaded.topology == MailTransferTopology::CrossServerImport);
    CHECK(loaded.status == MailTransferStatus::Preparing);
    CHECK(loaded.createdAt.isValid());
    CHECK(loaded.updatedAt.isValid());

    const auto loadedItems = requireItems(repository.listItems(transfer.operationId));
    REQUIRE(loadedItems.size() == 2);
    CHECK(loadedItems[0].ordinal == 0);
    CHECK(loadedItems[0].sourceEmailId == "email-a");
    CHECK(loadedItems[0].sourceBlobId == "source-blob-0");
    CHECK(loadedItems[0].sourceMailboxIds == std::vector<std::string>{"inbox", "important"});
    CHECK(loadedItems[0].sourceKeywords ==
          std::vector<std::string>{"$seen", "$flagged", "custom-keyword"});
    CHECK(loadedItems[0].sourceRemoveMailboxIds == std::vector<std::string>{"inbox"});
    CHECK_FALSE(loadedItems[0].sourceDestroy);
    CHECK(loadedItems[0].phase == MailTransferItemPhase::Prepared);
    CHECK(loadedItems[1].ordinal == 1);
    CHECK(loadedItems[1].sourceEmailId == "email-b");
}

TEST_CASE("mail transfer journal records every irreversible boundary with compare and swap",
          "[app][mail-transfer][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto database = openDatabase(directory.filePath(QStringLiteral("transfer.sqlite3")));
    MailTransferRepository repository{database};

    const auto transfer = operation();
    REQUIRE_FALSE(repository.create(transfer, {item(transfer.operationId, "item-1", 0, "email-a")})
                      .has_value());
    const auto sourceObject = seedVaultObject(database);

    CHECK(requireBool(repository.transitionItem("item-1", MailTransferItemPhase::Prepared,
                                                MailTransferItemPhase::AcquiringSource)));
    CHECK_FALSE(requireBool(repository.transitionItem("item-1", MailTransferItemPhase::Prepared,
                                                      MailTransferItemPhase::AcquiringSource)));
    CHECK(requireBool(repository.markSourceReady("item-1", MailTransferItemPhase::AcquiringSource,
                                                 sourceObject.contentHash)));
    CHECK(scalar(database, QStringLiteral(
                              "SELECT COUNT(*) FROM mail_vault_pins WHERE "
                              "owner_kind='mail_transfer_item' AND owner_id='item-1'")) == 1);
    CHECK(requireBool(repository.transitionItem("item-1", MailTransferItemPhase::SourceReady,
                                                MailTransferItemPhase::Uploading)));
    CHECK(requireBool(repository.markUploaded("item-1", MailTransferItemPhase::Uploading,
                                              "destination-upload-blob")));
    CHECK(requireBool(repository.markDestinationDispatching(
        "item-1", MailTransferItemPhase::Uploaded, "destination-state-before-import")));

    CHECK(requireBool(repository.markDestinationConfirmed(
        "item-1", MailTransferItemPhase::CreatingDestination,
        {
            .emailId = "destination-email",
            .blobId = std::optional<std::string>{"destination-blob"},
            .threadId = std::optional<std::string>{"destination-thread"},
            .size = std::optional<std::uint64_t>{4096},
            .reusedExisting = true,
            .priorMailboxIds = std::optional<std::vector<std::string>>{{"old-mailbox"}},
        })));
    CHECK_FALSE(requireBool(
        repository.markDestinationConfirmed("item-1", MailTransferItemPhase::CreatingDestination,
                                            {
                                                .emailId = "duplicate-confirmation",
                                                .blobId = std::nullopt,
                                                .threadId = std::nullopt,
                                                .size = std::nullopt,
                                                .reusedExisting = false,
                                                .priorMailboxIds = std::nullopt,
                                            })));

    const auto items = requireItems(repository.listItems(transfer.operationId));
    REQUIRE(items.size() == 1);
    const auto& stored = items.front();
    CHECK(stored.rawContentHash == std::optional<std::string>{sourceObject.contentHash});
    CHECK(stored.destinationUploadBlobId == std::optional<std::string>{"destination-upload-blob"});
    CHECK(stored.destinationPreState ==
          std::optional<std::string>{"destination-state-before-import"});
    CHECK(stored.destinationEmailId == std::optional<std::string>{"destination-email"});
    CHECK(stored.destinationBlobId == std::optional<std::string>{"destination-blob"});
    CHECK(stored.destinationThreadId == std::optional<std::string>{"destination-thread"});
    CHECK(stored.destinationSize == std::optional<std::uint64_t>{4096});
    CHECK(stored.reusedExisting);
    CHECK(stored.destinationPriorMailboxIds ==
          std::optional<std::vector<std::string>>{{"old-mailbox"}});
    CHECK(stored.phase == MailTransferItemPhase::DestinationConfirmed);
}

TEST_CASE("mail transfer source pin is atomic and can be retained by history",
          "[app][mail-transfer][repository][vault]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto database = openDatabase(directory.filePath(QStringLiteral("transfer.sqlite3")));
    MailTransferRepository repository{database};

    const auto transfer = operation();
    REQUIRE_FALSE(repository.create(transfer, {item(transfer.operationId, "item-1", 0, "email-a")})
                      .has_value());
    REQUIRE(requireBool(repository.transitionItem("item-1", MailTransferItemPhase::Prepared,
                                                  MailTransferItemPhase::AcquiringSource)));

    const auto rejected = repository.markSourceReady(
        "item-1", MailTransferItemPhase::AcquiringSource, "missing-vault-object");
    REQUIRE(std::holds_alternative<DatabaseError>(rejected));
    auto afterRejectedPin = requireItems(repository.listItems(transfer.operationId));
    REQUIRE(afterRejectedPin.size() == 1);
    CHECK(afterRejectedPin.front().phase == MailTransferItemPhase::AcquiringSource);
    CHECK_FALSE(afterRejectedPin.front().rawContentHash.has_value());
    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM mail_vault_pins")) == 0);

    const auto sourceObject = seedVaultObject(database);
    REQUIRE(requireBool(repository.markSourceReady(
        "item-1", MailTransferItemPhase::AcquiringSource, sourceObject.contentHash)));
    REQUIRE(requireBool(
        repository.reassignSourcePin("item-1", "operation_history", "history-entry-1")));
    CHECK(scalar(database, QStringLiteral(
                              "SELECT COUNT(*) FROM mail_vault_pins WHERE "
                              "owner_kind='mail_transfer_item' AND owner_id='item-1'")) == 0);
    CHECK(scalar(database, QStringLiteral(
                              "SELECT COUNT(*) FROM mail_vault_pins WHERE "
                              "owner_kind='operation_history' AND owner_id='history-entry-1'")) == 1);
    REQUIRE(requireBool(
        repository.reassignSourcePin("item-1", "operation_history", "history-entry-1")));
    CHECK(scalar(database, QStringLiteral(
                              "SELECT COUNT(*) FROM mail_vault_pins WHERE "
                              "owner_kind='operation_history' AND owner_id='history-entry-1'")) == 1);
    REQUIRE_FALSE(repository.releaseSourcePin("item-1").has_value());
    CHECK(scalar(database, QStringLiteral("SELECT COUNT(*) FROM mail_vault_pins")) == 1);
}

TEST_CASE("mail transfer journal survives restart and exposes recoverable operations",
          "[app][mail-transfer][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("transfer.sqlite3"));

    {
        auto database = openDatabase(path);
        MailTransferRepository repository{database};
        auto transfer = operation();
        transfer.status = MailTransferStatus::Running;
        REQUIRE_FALSE(
            repository.create(transfer, {item(transfer.operationId, "item-1", 0, "email-a")})
                .has_value());
        REQUIRE_FALSE(repository.updateStatus(transfer.operationId,
                                              MailTransferStatus::BlockedUnknown,
                                              QStringLiteral("destination outcome unknown")));
        CHECK(requireBool(repository.transitionItem("item-1", MailTransferItemPhase::Prepared,
                                                    MailTransferItemPhase::CreatingDestination)));
        CHECK(requireBool(
            repository.transitionItem("item-1", MailTransferItemPhase::CreatingDestination,
                                      MailTransferItemPhase::DestinationUnknown,
                                      QStringLiteral("connection lost after dispatch"))));
    }

    auto database = openDatabase(path);
    MailTransferRepository repository{database};
    auto recoverableResult = repository.listRecoverable();
    REQUIRE(std::holds_alternative<std::vector<MailTransferOperationRecord>>(recoverableResult));
    const auto& recoverable = std::get<std::vector<MailTransferOperationRecord>>(recoverableResult);
    REQUIRE(recoverable.size() == 1);
    CHECK(recoverable.front().operationId == "transfer-1");
    CHECK(recoverable.front().status == MailTransferStatus::BlockedUnknown);
    CHECK(recoverable.front().lastError ==
          std::optional<QString>{QStringLiteral("destination outcome unknown")});

    const auto items = requireItems(repository.listItems("transfer-1"));
    REQUIRE(items.size() == 1);
    CHECK(items.front().phase == MailTransferItemPhase::DestinationUnknown);
    CHECK(items.front().lastError ==
          std::optional<QString>{QStringLiteral("connection lost after dispatch")});
    CHECK(items.front().createdAt.isValid());
    CHECK(items.front().updatedAt.isValid());
}

TEST_CASE("mail transfer operation and items are inserted atomically",
          "[app][mail-transfer][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto database = openDatabase(directory.filePath(QStringLiteral("transfer.sqlite3")));
    MailTransferRepository repository{database};

    const auto transfer = operation();
    auto first = item(transfer.operationId, "item-1", 0, "email-a");
    auto duplicateOrdinal = item(transfer.operationId, "item-2", 0, "email-b");
    REQUIRE(repository.create(transfer, {first, duplicateOrdinal}).has_value());

    const auto missing = repository.findOperation(transfer.operationId);
    REQUIRE(std::holds_alternative<std::optional<MailTransferOperationRecord>>(missing));
    CHECK_FALSE(std::get<std::optional<MailTransferOperationRecord>>(missing).has_value());
    CHECK(requireItems(repository.listItems(transfer.operationId)).empty());
}
