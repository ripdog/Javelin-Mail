#include "app/MailTransferApplicationService.h"
#include "app/MailTransferRepository.h"
#include "jmap/api/Session.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/SyncStateRepository.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace
{
    using namespace javelin::app;

    class ApplicationGuard
    {
      public:
        ApplicationGuard()
        {
            if (QCoreApplication::instance() != nullptr)
                return;
            static int argc = 1;
            static char name[] = "mail-transfer-application-service-test";
            static char* argv[]{name, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    [[nodiscard]] javelin::jmap::cache::DatabaseConnection database(const QString& path)
    {
        auto opened = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = QStringLiteral("mail-transfer-service-%1")
                                  .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)),
            .databasePath = path,
        });
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
            FAIL(error->message.toStdString());
        return std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    }

    [[nodiscard]] javelin::jmap::api::Account sessionAccount(std::string id)
    {
        return {
            .id = std::move(id),
            .name = "Mail",
            .isPersonal = true,
            .isReadOnly = false,
            .accountCapabilities =
                {
                    .mail = true,
                    .mailDetails =
                        javelin::jmap::api::MailAccountCapability{.mayCreateTopLevelMailbox = true},
                    .submission = std::nullopt,
                    .contacts = std::nullopt,
                    .calendars = std::nullopt,
                    .sieve = false,
                },
        };
    }

    [[nodiscard]] javelin::jmap::api::Session session(std::vector<std::string> accountIds,
                                                      std::string primary)
    {
        javelin::jmap::api::Session value{
            .username = "alice@example.test",
            .apiUrl = "https://example.test/jmap",
            .downloadUrl = "https://example.test/download/{accountId}/{blobId}/{name}",
            .uploadUrl = "https://example.test/upload/{accountId}",
            .eventSourceUrl = std::nullopt,
            .state = "session-state",
            .capabilities =
                {
                    .core = true,
                    .coreDetails =
                        javelin::jmap::api::CoreCapability{
                            .maxSizeUpload = 64 * 1024 * 1024,
                            .maxConcurrentUpload = 2,
                            .maxSizeRequest = 1024 * 1024,
                            .maxConcurrentRequests = 4,
                            .maxCallsInRequest = 8,
                            .maxObjectsInGet = 256,
                            .maxObjectsInSet = 128,
                            .collationAlgorithms = {},
                        },
                    .mail = true,
                    .submission = false,
                    .contacts = false,
                    .calendars = false,
                    .sieve = false,
                    .websocket = std::nullopt,
                },
            .accounts = {},
            .primaryAccounts = {.mailAccountId = primary,
                                .submissionAccountId = std::nullopt,
                                .contactsAccountId = std::nullopt,
                                .calendarsAccountId = std::nullopt,
                                .sieveAccountId = std::nullopt},
        };
        for (const auto& id : accountIds)
            value.accounts.emplace(id, sessionAccount(id));
        return value;
    }

    [[nodiscard]] std::string storeSession(javelin::jmap::cache::DatabaseConnection& database,
                                           std::string connectionId, std::string remoteAccountId,
                                           javelin::jmap::api::Session value)
    {
        javelin::jmap::cache::SessionRepository sessions{database};
        const auto stored = sessions.replaceForConnection(connectionId, remoteAccountId, value);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&stored))
            FAIL(error->message.toStdString());
        const auto& mapping =
            std::get<javelin::jmap::cache::StoredSessionAccounts>(stored).accountIdsByRemoteId;
        const auto found = mapping.find(remoteAccountId);
        REQUIRE(found != mapping.end());
        return found->second;
    }

    [[nodiscard]] javelin::jmap::domain::Mailbox mailbox(std::string id, std::string name)
    {
        return {
            .id = std::move(id),
            .name = std::move(name),
            .parentId = std::nullopt,
            .role = std::nullopt,
            .sortOrder = 0,
            .totalEmails = 1,
            .unreadEmails = 0,
            .totalThreads = 1,
            .unreadThreads = 0,
            .isSubscribed = true,
            .myRights = {.mayReadItems = true,
                         .mayAddItems = true,
                         .mayRemoveItems = true,
                         .maySetSeen = true,
                         .maySetKeywords = true,
                         .mayCreateChild = true,
                         .mayRename = true,
                         .mayDelete = true,
                         .maySubmit = true},
        };
    }

    [[nodiscard]] javelin::jmap::domain::Email email()
    {
        return {
            .id = "email-1",
            .blobId = "blob-1",
            .threadId = "thread-1",
            .mailboxIds = {"inbox"},
            .keywords = {"$seen", "$flagged", "project"},
            .size = 8192,
            .receivedAt = "2026-08-15T08:00:00Z",
            .sentAt = std::nullopt,
            .messageId = {"message@example.test"},
            .inReplyTo = {},
            .references = {},
            .hasAttachment = true,
            .subject = "Transfer me",
            .from = {},
            .to = {},
            .cc = {},
            .bcc = {},
            .replyTo = {},
            .preview = "Preview",
        };
    }

    [[nodiscard]] std::vector<MailTransferItemRecord>
    requireItems(std::variant<std::vector<MailTransferItemRecord>,
                              javelin::jmap::cache::DatabaseError> result)
    {
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            FAIL(error->message.toStdString());
        return std::get<std::vector<MailTransferItemRecord>>(std::move(result));
    }
} // namespace

TEST_CASE("mail transfer preparation journals a cross-server collision safely",
          "[app][mail-transfer][preparation]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto db = database(directory.filePath(QStringLiteral("cache.sqlite3")));

    const auto sourceLocal = storeSession(db, "connection-a", "u1", session({"u1"}, "u1"));
    const auto destinationLocal = storeSession(db, "connection-b", "u1", session({"u1"}, "u1"));
    REQUIRE(sourceLocal != destinationLocal);

    javelin::jmap::cache::MailboxRepository mailboxes{db};
    REQUIRE_FALSE(mailboxes.replaceAll(sourceLocal, {mailbox("inbox", "Inbox")}).has_value());
    REQUIRE_FALSE(
        mailboxes.replaceAll(destinationLocal, {mailbox("archive", "Archive")}).has_value());
    const auto sourceEmail = email();
    javelin::jmap::cache::EmailRepository emails{db};
    REQUIRE_FALSE(emails.upsertMany(sourceLocal, {sourceEmail}).has_value());
    javelin::jmap::cache::SyncStateRepository states{db};
    REQUIRE_FALSE(states
                      .upsert({.accountId = sourceLocal, .objectType = "Email", .queryKey = {}},
                              "email-state-7")
                      .has_value());

    MailTransferApplicationService service{db};
    const auto prepared = QCoro::waitFor(service.prepare({
        .intent = {.sourceAccountId = sourceLocal,
                   .sourceMailboxId = std::optional<std::string>{"inbox"},
                   .destinationAccountId = destinationLocal,
                   .destinationMailboxId = "archive",
                   .operation = MailTransferOperation::Move},
        .selection = {SelectedEmail{.emailId = sourceEmail.id}},
    }));
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&prepared))
        FAIL(error->message.toStdString());
    const auto& summary = std::get<PreparedMailTransfer>(prepared);
    CHECK(summary.itemCount == 1);
    CHECK(summary.topology == MailTransferTopology::CrossServerImport);

    MailTransferRepository repository{db};
    const auto operationResult = repository.findOperation(summary.operationId);
    REQUIRE(std::holds_alternative<std::optional<MailTransferOperationRecord>>(operationResult));
    const auto& operation = std::get<std::optional<MailTransferOperationRecord>>(operationResult);
    REQUIRE(operation.has_value());
    CHECK(operation->operationGroupId == std::optional<std::string>{summary.operationId});
    CHECK(operation->sourceAccountId == sourceLocal);
    CHECK(operation->destinationAccountId == destinationLocal);
    CHECK(operation->status == MailTransferStatus::Preparing);
    CHECK(operation->topology == MailTransferTopology::CrossServerImport);

    const auto items = requireItems(repository.listItems(summary.operationId));
    REQUIRE(items.size() == 1);
    CHECK(items.front().sourceEmailId == sourceEmail.id);
    CHECK(items.front().sourceBlobId == sourceEmail.blobId);
    CHECK(items.front().sourceMailboxIds == sourceEmail.mailboxIds);
    CHECK(items.front().sourceKeywords == std::vector<std::string>{"$flagged", "$seen", "project"});
    CHECK(items.front().sourceMessageIds == std::vector<std::string>{"message@example.test"});
    CHECK(items.front().sourceEmailState == std::optional<std::string>{"email-state-7"});
    CHECK(items.front().sourceRemoveMailboxIds == std::vector<std::string>{"inbox"});
    CHECK(items.front().sourceDestroy);
    CHECK(items.front().phase == MailTransferItemPhase::Prepared);
    CHECK_FALSE(items.front().rawContentHash.has_value());
    CHECK_FALSE(items.front().destinationCreationId.empty());

    QSqlQuery pins{db.database()};
    REQUIRE(pins.exec(QStringLiteral("SELECT COUNT(*) FROM mail_vault_pins")));
    REQUIRE(pins.next());
    CHECK(pins.value(0).toInt() == 0);
}

TEST_CASE("mail transfer preparation verifies both accounts are in one cached JMAP session",
          "[app][mail-transfer][preparation]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto db = database(directory.filePath(QStringLiteral("cache.sqlite3")));

    const auto shared = session({"u1", "u2"}, "u1");
    javelin::jmap::cache::SessionRepository sessions{db};
    const auto stored = sessions.replaceForConnection("connection-a", "u1", shared);
    REQUIRE(std::holds_alternative<javelin::jmap::cache::StoredSessionAccounts>(stored));
    const auto& ids =
        std::get<javelin::jmap::cache::StoredSessionAccounts>(stored).accountIdsByRemoteId;
    const auto sourceLocal = ids.at("u1");
    const auto destinationLocal = ids.at("u2");

    javelin::jmap::cache::MailboxRepository mailboxes{db};
    REQUIRE_FALSE(mailboxes.replaceAll(sourceLocal, {mailbox("inbox", "Inbox")}).has_value());
    REQUIRE_FALSE(
        mailboxes.replaceAll(destinationLocal, {mailbox("archive", "Archive")}).has_value());
    javelin::jmap::cache::EmailRepository emails{db};
    const auto sourceEmail = email();
    REQUIRE_FALSE(emails.upsertMany(sourceLocal, {sourceEmail}).has_value());
    javelin::jmap::cache::SyncStateRepository states{db};
    REQUIRE_FALSE(states
                      .upsert({.accountId = sourceLocal, .objectType = "Email", .queryKey = {}},
                              "email-state-1")
                      .has_value());

    MailTransferApplicationService service{db};
    const auto prepared = QCoro::waitFor(service.prepare({
        .intent = {.sourceAccountId = sourceLocal,
                   .sourceMailboxId = std::optional<std::string>{"inbox"},
                   .destinationAccountId = destinationLocal,
                   .destinationMailboxId = "archive",
                   .operation = MailTransferOperation::Copy},
        .selection = {SelectedEmail{.emailId = sourceEmail.id}},
    }));
    REQUIRE(std::holds_alternative<PreparedMailTransfer>(prepared));
    CHECK(std::get<PreparedMailTransfer>(prepared).topology ==
          MailTransferTopology::SameSessionCopy);
}

TEST_CASE("mail transfer preparation requires an authoritative source Email state",
          "[app][mail-transfer][preparation]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto db = database(directory.filePath(QStringLiteral("cache.sqlite3")));

    const auto sourceLocal = storeSession(db, "connection-a", "u1", session({"u1"}, "u1"));
    const auto destinationLocal = storeSession(db, "connection-b", "u1", session({"u1"}, "u1"));
    javelin::jmap::cache::MailboxRepository mailboxes{db};
    REQUIRE_FALSE(mailboxes.replaceAll(sourceLocal, {mailbox("inbox", "Inbox")}).has_value());
    REQUIRE_FALSE(
        mailboxes.replaceAll(destinationLocal, {mailbox("archive", "Archive")}).has_value());
    javelin::jmap::cache::EmailRepository emails{db};
    const auto sourceEmail = email();
    REQUIRE_FALSE(emails.upsertMany(sourceLocal, {sourceEmail}).has_value());

    MailTransferApplicationService service{db};
    const auto prepared = QCoro::waitFor(service.prepare({
        .intent = {.sourceAccountId = sourceLocal,
                   .sourceMailboxId = std::optional<std::string>{"inbox"},
                   .destinationAccountId = destinationLocal,
                   .destinationMailboxId = "archive",
                   .operation = MailTransferOperation::Copy},
        .selection = {SelectedEmail{.emailId = sourceEmail.id}},
    }));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(prepared));
    MailTransferRepository repository{db};
    const auto recoverable = repository.listRecoverable();
    REQUIRE(std::holds_alternative<std::vector<MailTransferOperationRecord>>(recoverable));
    CHECK(std::get<std::vector<MailTransferOperationRecord>>(recoverable).empty());
}
