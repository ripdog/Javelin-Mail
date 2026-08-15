#include "app/MailTransferApplicationService.h"
#include "app/MailTransferExecutor.h"
#include "app/MailTransferRepository.h"
#include "app/AccountConnectionProvider.h"
#include "jmap/MessageContentClient.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/Session.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/RawMessageSourceRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/SyncStateRepository.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace
{
    using namespace javelin::app;
    using javelin::jmap::api::HttpRequest;
    using javelin::jmap::api::HttpResponse;
    using javelin::jmap::api::JmapMethodRequest;
    using javelin::jmap::api::JmapMethodTransportResult;
    using javelin::jmap::api::ResponseEnvelope;
    using javelin::jmap::api::TransportError;
    using javelin::jmap::api::TransportErrorCode;

    class ApplicationGuard
    {
      public:
        ApplicationGuard()
        {
            if (QCoreApplication::instance() != nullptr)
                return;
            static int argc = 1;
            static char name[] = "mail-transfer-executor-test";
            static char* argv[]{name, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    [[nodiscard]] javelin::jmap::cache::DatabaseConnection openDatabase(const QString& path)
    {
        auto opened = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = QStringLiteral("mail-transfer-executor-%1")
                                  .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)),
            .databasePath = path,
        });
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
            FAIL(error->message.toStdString());
        return std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    }

    [[nodiscard]] javelin::jmap::api::Session session(std::string apiUrl, std::string uploadUrl)
    {
        javelin::jmap::api::Session value{
            .username = "alice@example.test",
            .apiUrl = std::move(apiUrl),
            .downloadUrl = "https://example.test/download/{accountId}/{blobId}/{name}",
            .uploadUrl = std::move(uploadUrl),
            .eventSourceUrl = std::nullopt,
            .state = "session-state",
            .capabilities =
                {
                    .core = true,
                    .coreDetails = javelin::jmap::api::CoreCapability{
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
            .primaryAccounts = {.mailAccountId = "u1",
                                .submissionAccountId = std::nullopt,
                                .contactsAccountId = std::nullopt,
                                .calendarsAccountId = std::nullopt,
                                .sieveAccountId = std::nullopt},
        };
        value.accounts.emplace(
            "u1", javelin::jmap::api::Account{
                      .id = "u1",
                      .name = "Mail",
                      .isPersonal = true,
                      .isReadOnly = false,
                      .accountCapabilities =
                          {
                              .mail = true,
                              .mailDetails = javelin::jmap::api::MailAccountCapability{
                                  .mayCreateTopLevelMailbox = true},
                              .submission = std::nullopt,
                              .contacts = std::nullopt,
                              .calendars = std::nullopt,
                              .sieve = false,
                          },
                  });
        return value;
    }

    [[nodiscard]] std::string storeSession(javelin::jmap::cache::DatabaseConnection& database,
                                           std::string connectionId,
                                           javelin::jmap::api::Session value)
    {
        javelin::jmap::cache::SessionRepository sessions{database};
        const auto stored = sessions.replaceForConnection(connectionId, "u1", value);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&stored))
            FAIL(error->message.toStdString());
        return std::get<javelin::jmap::cache::StoredSessionAccounts>(stored)
            .accountIdsByRemoteId.at("u1");
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
            .keywords = {"$seen", "$flagged"},
            .size = 2048,
            .receivedAt = "2026-08-15T08:00:00Z",
            .sentAt = std::nullopt,
            .messageId = {"mail-transfer@example.test"},
            .inReplyTo = {},
            .references = {},
            .hasAttachment = false,
            .subject = "Transfer",
            .from = {},
            .to = {},
            .cc = {},
            .bcc = {},
            .replyTo = {},
            .preview = "Preview",
        };
    }

    class ConnectionProvider final : public AccountConnectionProvider
    {
      public:
        std::unordered_map<std::string, AccountConnectionSettings> settings;

        [[nodiscard]] std::optional<AccountConnectionSettings>
        connectionSettingsFor(std::string_view ownerAccountId) const override
        {
            const auto found = settings.find(std::string{ownerAccountId});
            return found == settings.end() ? std::nullopt : std::optional{found->second};
        }
    };

    class RecordingResourceTransport final : public javelin::jmap::api::AbstractTransport
    {
      public:
        QByteArray uploadedPayload;
        int sendCalls = 0;
        int sendFromFileCalls = 0;

        [[nodiscard]] QCoro::Task<javelin::jmap::api::TransportResult>
        send(HttpRequest request) override
        {
            ++sendCalls;
            static_cast<void>(request);
            co_return TransportError{
                .code = TransportErrorCode::NetworkFailure,
                .message = "Unexpected non-file HTTP request",
                .httpStatus = std::nullopt,
                .networkError = std::nullopt,
                .retryAfter = std::nullopt,
            };
        }

        [[nodiscard]] QCoro::Task<javelin::jmap::api::TransportResult>
        sendFromFile(HttpRequest request, QString filePath) override
        {
            ++sendFromFileCalls;
            if (request.dispatched)
                request.dispatched();
            QFile file{filePath};
            REQUIRE(file.open(QIODevice::ReadOnly));
            uploadedPayload = file.readAll();
            co_return HttpResponse{
                .statusCode = 200,
                .body = QByteArrayLiteral(
                    R"({"accountId":"u1","blobId":"uploaded-blob","type":"message/rfc822","size":37})"),
            };
        }
    };

    enum class ImportBehavior
    {
        Success,
        Reject,
        DispatchedFailure,
        PreDispatchFailure,
        AlreadyExistsInTarget,
        AlreadyExistsNeedsMembership,
        DuplicateMembershipDispatchedFailure,
    };

    class RecordingMethodTransport final : public javelin::jmap::api::JmapMethodTransport
    {
      public:
        ImportBehavior behavior = ImportBehavior::Success;
        int stateCalls = 0;
        int existingGetCalls = 0;
        int importCalls = 0;
        int setCalls = 0;
        std::string importArguments;
        std::string setArguments;

        [[nodiscard]] QCoro::Task<JmapMethodTransportResult>
        call(JmapMethodRequest request) override
        {
            REQUIRE(request.envelope.methodCalls.size() == 1);
            const auto& method = request.envelope.methodCalls.front();
            if (method.name == "Email/get")
            {
                if (request.dispatched)
                    request.dispatched();
                if (method.arguments.find("existing-email") != std::string::npos)
                {
                    ++existingGetCalls;
                    const bool inTarget = behavior == ImportBehavior::AlreadyExistsInTarget;
                    const auto mailboxIds =
                        inTarget ? R"({"archive":true,"old-mailbox":true})"
                                 : R"({"old-mailbox":true})";
                    const auto response =
                        std::string{R"({"accountId":"u1","state":"destination-state-existing","list":[{"id":"existing-email","blobId":"existing-blob","threadId":"existing-thread","mailboxIds":)"} +
                        mailboxIds +
                        R"(,"keywords":{"destination-tag":true},"size":91,"receivedAt":"2026-08-14T00:00:00Z"}],"notFound":[]})";
                    co_return ResponseEnvelope{
                        .methodResponses = {{.name = "Email/get",
                                             .arguments = response,
                                             .callId = method.callId}},
                        .createdIds = std::nullopt,
                        .sessionState = "session-state",
                    };
                }

                ++stateCalls;
                co_return ResponseEnvelope{
                    .methodResponses = {{.name = "Email/get",
                                         .arguments =
                                             R"({"accountId":"u1","state":"destination-state-1","list":[],"notFound":[]})",
                                         .callId = method.callId}},
                    .createdIds = std::nullopt,
                    .sessionState = "session-state",
                };
            }

            if (method.name == "Email/set")
            {
                ++setCalls;
                setArguments = method.arguments;
                if (request.dispatched)
                    request.dispatched();
                if (behavior == ImportBehavior::DuplicateMembershipDispatchedFailure)
                {
                    co_return TransportError{
                        .code = TransportErrorCode::NetworkFailure,
                        .message = "connection lost after duplicate membership dispatch",
                        .httpStatus = std::nullopt,
                        .networkError = std::nullopt,
                        .retryAfter = std::nullopt,
                    };
                }
                co_return ResponseEnvelope{
                    .methodResponses = {{
                        .name = "Email/set",
                        .arguments =
                            R"({"accountId":"u1","oldState":"destination-state-existing","newState":"destination-state-membership","created":{},"updated":{"existing-email":null},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{}})",
                        .callId = method.callId,
                    }},
                    .createdIds = std::nullopt,
                    .sessionState = "session-state",
                };
            }

            REQUIRE(method.name == "Email/import");
            ++importCalls;
            importArguments = method.arguments;
            if (behavior == ImportBehavior::PreDispatchFailure)
            {
                co_return TransportError{
                    .code = TransportErrorCode::NetworkFailure,
                    .message = "offline before dispatch",
                    .httpStatus = std::nullopt,
                    .networkError = std::nullopt,
                    .retryAfter = std::nullopt,
                };
            }
            if (request.dispatched)
                request.dispatched();
            if (behavior == ImportBehavior::DispatchedFailure)
            {
                co_return TransportError{
                    .code = TransportErrorCode::NetworkFailure,
                    .message = "connection lost after dispatch",
                    .httpStatus = std::nullopt,
                    .networkError = std::nullopt,
                    .retryAfter = std::nullopt,
                };
            }

            const auto arguments =
                QJsonDocument::fromJson(QByteArray::fromStdString(method.arguments)).object();
            const auto emails = arguments.value(QStringLiteral("emails")).toObject();
            REQUIRE(emails.size() == 1);
            const auto creationId = emails.begin().key();
            if (behavior == ImportBehavior::Reject)
            {
                const auto response = QStringLiteral(
                                          R"({"accountId":"u1","oldState":"destination-state-1","newState":"destination-state-1","created":{},"notCreated":{"%1":{"type":"overQuota","description":"Mailbox quota exceeded","properties":[]}}})")
                                          .arg(creationId)
                                          .toStdString();
                co_return ResponseEnvelope{
                    .methodResponses = {{.name = "Email/import",
                                         .arguments = response,
                                         .callId = method.callId}},
                    .createdIds = std::nullopt,
                    .sessionState = "session-state",
                };
            }
            if (behavior == ImportBehavior::AlreadyExistsInTarget ||
                behavior == ImportBehavior::AlreadyExistsNeedsMembership ||
                behavior == ImportBehavior::DuplicateMembershipDispatchedFailure)
            {
                const auto response = QStringLiteral(
                                          R"({"accountId":"u1","oldState":"destination-state-1","newState":"destination-state-1","created":{},"notCreated":{"%1":{"type":"alreadyExists","existingId":"existing-email","properties":[]}}})")
                                          .arg(creationId)
                                          .toStdString();
                co_return ResponseEnvelope{
                    .methodResponses = {{.name = "Email/import",
                                         .arguments = response,
                                         .callId = method.callId}},
                    .createdIds = std::nullopt,
                    .sessionState = "session-state",
                };
            }

            const auto response = QStringLiteral(
                                      R"({"accountId":"u1","oldState":"destination-state-1","newState":"destination-state-2","created":{"%1":{"id":"destination-email","blobId":"destination-blob","threadId":"destination-thread","size":37}},"notCreated":{}})")
                                      .arg(creationId)
                                      .toStdString();
            co_return ResponseEnvelope{
                .methodResponses = {{.name = "Email/import",
                                     .arguments = response,
                                     .callId = method.callId}},
                .createdIds = std::nullopt,
                .sessionState = "session-state",
            };
        }
    };

    struct Fixture
    {
        QTemporaryDir directory;
        javelin::jmap::cache::DatabaseConnection database;
        std::string sourceAccountId;
        std::string destinationAccountId;
        ConnectionProvider connections;
        RecordingResourceTransport resourceTransport;
        RecordingMethodTransport methodTransport;
        std::unique_ptr<javelin::jmap::MessageContentClient> contentClient;
        QByteArray raw = QByteArrayLiteral("From: sender@example.test\r\nSubject: Transfer\r\n\r\nBody\r\n");

        Fixture()
            : database(openDatabase([this]
                                    {
                                        REQUIRE(directory.isValid());
                                        return directory.filePath(QStringLiteral("cache.sqlite3"));
                                    }()))
        {
            sourceAccountId = storeSession(
                database, "connection-a",
                session("https://source.example.test/jmap",
                        "https://source.example.test/upload/{accountId}"));
            destinationAccountId = storeSession(
                database, "connection-b",
                session("https://destination.example.test/jmap",
                        "https://destination.example.test/upload/{accountId}"));
            REQUIRE(sourceAccountId != destinationAccountId);

            connections.settings.emplace(
                sourceAccountId,
                AccountConnectionSettings{.connectionId = "connection-a",
                                          .revision = 1,
                                          .displayName = "Source",
                                          .sessionUrl = "https://source.example.test/session",
                                          .loginEmail = "source@example.test",
                                          .apiKey = "source-token",
                                          .refreshToken = {},
                                          .tokenEndpoint = {},
                                          .oauthClientId = {},
                                          .oauthIssuer = {},
                                          .oauthResource = {},
                                          .oauthScope = {},
                                          .revocationEndpoint = {},
                                          .registrationClientUri = {},
                                          .registrationAccessToken = {}});
            connections.settings.emplace(
                destinationAccountId,
                AccountConnectionSettings{.connectionId = "connection-b",
                                          .revision = 1,
                                          .displayName = "Destination",
                                          .sessionUrl = "https://destination.example.test/session",
                                          .loginEmail = "destination@example.test",
                                          .apiKey = "destination-token",
                                          .refreshToken = {},
                                          .tokenEndpoint = {},
                                          .oauthClientId = {},
                                          .oauthIssuer = {},
                                          .oauthResource = {},
                                          .oauthScope = {},
                                          .revocationEndpoint = {},
                                          .registrationClientUri = {},
                                          .registrationAccessToken = {}});

            javelin::jmap::cache::MailboxRepository mailboxes{database};
            REQUIRE_FALSE(
                mailboxes.replaceAll(sourceAccountId, {mailbox("inbox", "Inbox")}).has_value());
            REQUIRE_FALSE(mailboxes
                              .replaceAll(destinationAccountId, {mailbox("archive", "Archive")})
                              .has_value());
            javelin::jmap::cache::EmailRepository emails{database};
            REQUIRE_FALSE(emails.upsertMany(sourceAccountId, {email()}).has_value());
            javelin::jmap::cache::SyncStateRepository states{database};
            REQUIRE_FALSE(states
                              .upsert({.accountId = sourceAccountId,
                                       .objectType = "Email",
                                       .queryKey = {}},
                                      "source-email-state")
                              .has_value());
            javelin::jmap::cache::RawMessageSourceRepository sources{database};
            REQUIRE_FALSE(sources
                              .upsert(sourceAccountId,
                                      {.emailId = "email-1", .blobId = "blob-1", .payload = raw})
                              .has_value());
            contentClient = std::make_unique<javelin::jmap::MessageContentClient>(database,
                                                                                  resourceTransport);
        }

        [[nodiscard]] std::string prepare(MailTransferOperation operation)
        {
            MailTransferApplicationService service{database};
            const auto prepared = QCoro::waitFor(service.prepare({
                .intent = {.sourceAccountId = sourceAccountId,
                           .sourceMailboxId = std::optional<std::string>{"inbox"},
                           .destinationAccountId = destinationAccountId,
                           .destinationMailboxId = "archive",
                           .operation = operation},
                .selection = {SelectedEmail{.emailId = "email-1"}},
            }));
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&prepared))
                FAIL(error->message.toStdString());
            return std::get<PreparedMailTransfer>(prepared).operationId;
        }

        [[nodiscard]] MailTransferItemRecord item(const std::string& operationId)
        {
            MailTransferRepository repository{database};
            const auto result = repository.listItems(operationId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                FAIL(error->message.toStdString());
            const auto& items = std::get<std::vector<MailTransferItemRecord>>(result);
            REQUIRE(items.size() == 1);
            return items.front();
        }

        [[nodiscard]] MailTransferOperationRecord operation(const std::string& operationId)
        {
            MailTransferRepository repository{database};
            const auto result = repository.findOperation(operationId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                FAIL(error->message.toStdString());
            const auto& value = std::get<std::optional<MailTransferOperationRecord>>(result);
            REQUIRE(value.has_value());
            return *value;
        }

        [[nodiscard]] int pinCount() const
        {
            QSqlQuery query{database.database()};
            REQUIRE(query.exec(QStringLiteral("SELECT COUNT(*) FROM mail_vault_pins")));
            REQUIRE(query.next());
            return query.value(0).toInt();
        }

        [[nodiscard]] bool sourceStillExists() const
        {
            javelin::jmap::cache::EmailRepository emails{
                const_cast<javelin::jmap::cache::DatabaseConnection&>(database)};
            const auto result = emails.find(sourceAccountId, "email-1");
            REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(result));
            return std::get<std::optional<javelin::jmap::domain::Email>>(result).has_value();
        }

        [[nodiscard]] MailTransferExecutionResult execute(const std::string& operationId)
        {
            MailTransferExecutor executor{database, resourceTransport, methodTransport,
                                          *contentClient, connections};
            return QCoro::waitFor(executor.advance(operationId));
        }
    };
} // namespace

TEST_CASE("cross-server copy streams exact raw MIME and completes only after import confirmation",
          "[app][mail-transfer][executor]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    const auto operationId = fixture.prepare(MailTransferOperation::Copy);

    const auto result = fixture.execute(operationId);
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        FAIL(error->message.toStdString());
    const auto& summary = std::get<MailTransferExecutionSummary>(result);
    CHECK(summary.status == MailTransferStatus::Complete);
    CHECK(summary.completeItemCount == 1);
    CHECK(fixture.resourceTransport.sendCalls == 0);
    CHECK(fixture.resourceTransport.sendFromFileCalls == 1);
    CHECK(fixture.resourceTransport.uploadedPayload == fixture.raw);
    CHECK(fixture.methodTransport.stateCalls == 1);
    CHECK(fixture.methodTransport.importCalls == 1);
    CHECK(fixture.methodTransport.importArguments.find("\"archive\":true") != std::string::npos);
    CHECK(fixture.methodTransport.importArguments.find("\"$seen\":true") != std::string::npos);
    CHECK(fixture.methodTransport.importArguments.find("\"$flagged\":true") != std::string::npos);
    CHECK(fixture.methodTransport.importArguments.find(
              "\"ifInState\":\"destination-state-1\"") != std::string::npos);

    const auto item = fixture.item(operationId);
    CHECK(item.phase == MailTransferItemPhase::Complete);
    CHECK(item.destinationPreState == std::optional<std::string>{"destination-state-1"});
    CHECK(item.destinationEmailId == std::optional<std::string>{"destination-email"});
    CHECK(item.destinationBlobId == std::optional<std::string>{"destination-blob"});
    CHECK(item.destinationThreadId == std::optional<std::string>{"destination-thread"});
    CHECK(fixture.pinCount() == 0);
    CHECK(fixture.sourceStillExists());
}

TEST_CASE("cross-server move stops after durable destination confirmation and keeps source pinned",
          "[app][mail-transfer][executor][move]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    const auto operationId = fixture.prepare(MailTransferOperation::Move);

    const auto result = fixture.execute(operationId);
    REQUIRE(std::holds_alternative<MailTransferExecutionSummary>(result));
    const auto& summary = std::get<MailTransferExecutionSummary>(result);
    CHECK(summary.status == MailTransferStatus::Running);
    CHECK(summary.destinationConfirmedItemCount == 1);
    const auto item = fixture.item(operationId);
    CHECK(item.phase == MailTransferItemPhase::DestinationConfirmed);
    CHECK(item.sourceDestroy);
    CHECK(fixture.pinCount() == 1);
    CHECK(fixture.sourceStillExists());
}

TEST_CASE("definitive Email import rejection fails item and never removes the source",
          "[app][mail-transfer][executor][rejection]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    fixture.methodTransport.behavior = ImportBehavior::Reject;
    const auto operationId = fixture.prepare(MailTransferOperation::Move);

    const auto result = fixture.execute(operationId);
    REQUIRE(std::holds_alternative<MailTransferExecutionSummary>(result));
    CHECK(std::get<MailTransferExecutionSummary>(result).status == MailTransferStatus::Failed);
    const auto item = fixture.item(operationId);
    CHECK(item.phase == MailTransferItemPhase::Failed);
    REQUIRE(item.lastError.has_value());
    CHECK(item.lastError->contains(QStringLiteral("quota"), Qt::CaseInsensitive));
    CHECK(fixture.pinCount() == 0);
    CHECK(fixture.sourceStillExists());
}

TEST_CASE("connection loss after Email import dispatch becomes blocked unknown without retry",
          "[app][mail-transfer][executor][ambiguity]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    fixture.methodTransport.behavior = ImportBehavior::DispatchedFailure;
    const auto operationId = fixture.prepare(MailTransferOperation::Move);

    const auto first = fixture.execute(operationId);
    REQUIRE(std::holds_alternative<MailTransferExecutionSummary>(first));
    CHECK(std::get<MailTransferExecutionSummary>(first).status ==
          MailTransferStatus::BlockedUnknown);
    CHECK(fixture.item(operationId).phase == MailTransferItemPhase::DestinationUnknown);
    CHECK(fixture.operation(operationId).status == MailTransferStatus::BlockedUnknown);
    CHECK(fixture.pinCount() == 1);
    CHECK(fixture.sourceStillExists());
    const int uploadCount = fixture.resourceTransport.sendFromFileCalls;
    const int importCount = fixture.methodTransport.importCalls;

    fixture.methodTransport.behavior = ImportBehavior::Success;
    const auto second = fixture.execute(operationId);
    REQUIRE(std::holds_alternative<MailTransferExecutionSummary>(second));
    CHECK(std::get<MailTransferExecutionSummary>(second).status ==
          MailTransferStatus::BlockedUnknown);
    CHECK(fixture.resourceTransport.sendFromFileCalls == uploadCount);
    CHECK(fixture.methodTransport.importCalls == importCount);
    CHECK(fixture.sourceStillExists());
}

TEST_CASE("failure before Email import dispatch returns to uploaded phase and retries without reupload",
          "[app][mail-transfer][executor][retry]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    fixture.methodTransport.behavior = ImportBehavior::PreDispatchFailure;
    const auto operationId = fixture.prepare(MailTransferOperation::Copy);

    const auto first = fixture.execute(operationId);
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(first));
    CHECK(fixture.item(operationId).phase == MailTransferItemPhase::Uploaded);
    CHECK(fixture.operation(operationId).status == MailTransferStatus::WaitingForNetwork);
    CHECK(fixture.pinCount() == 1);
    CHECK(fixture.resourceTransport.sendFromFileCalls == 1);

    fixture.methodTransport.behavior = ImportBehavior::Success;
    const auto second = fixture.execute(operationId);
    REQUIRE(std::holds_alternative<MailTransferExecutionSummary>(second));
    CHECK(std::get<MailTransferExecutionSummary>(second).status == MailTransferStatus::Complete);
    CHECK(fixture.resourceTransport.sendFromFileCalls == 1);
    CHECK(fixture.methodTransport.importCalls == 2);
    CHECK(fixture.pinCount() == 0);
}

TEST_CASE("already-existing destination in target mailbox is reused without changing its state",
          "[app][mail-transfer][executor][duplicate]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    fixture.methodTransport.behavior = ImportBehavior::AlreadyExistsInTarget;
    const auto operationId = fixture.prepare(MailTransferOperation::Copy);

    const auto result = fixture.execute(operationId);
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        FAIL(error->message.toStdString());
    REQUIRE(std::holds_alternative<MailTransferExecutionSummary>(result));
    CHECK(std::get<MailTransferExecutionSummary>(result).status == MailTransferStatus::Complete);
    CHECK(fixture.methodTransport.existingGetCalls == 1);
    CHECK(fixture.methodTransport.setCalls == 0);
    const auto item = fixture.item(operationId);
    CHECK(item.phase == MailTransferItemPhase::Complete);
    CHECK(item.reusedExisting);
    CHECK(item.destinationEmailId == std::optional<std::string>{"existing-email"});
    CHECK(item.destinationBlobId == std::optional<std::string>{"existing-blob"});
    CHECK(item.destinationThreadId == std::optional<std::string>{"existing-thread"});
    CHECK(item.destinationPriorMailboxIds ==
          std::optional<std::vector<std::string>>{{"archive", "old-mailbox"}});
    CHECK(fixture.pinCount() == 0);
    CHECK(fixture.sourceStillExists());
}

TEST_CASE("already-existing destination outside target gains only target mailbox membership",
          "[app][mail-transfer][executor][duplicate]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    fixture.methodTransport.behavior = ImportBehavior::AlreadyExistsNeedsMembership;
    const auto operationId = fixture.prepare(MailTransferOperation::Copy);

    const auto result = fixture.execute(operationId);
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        FAIL(error->message.toStdString());
    REQUIRE(std::holds_alternative<MailTransferExecutionSummary>(result));
    CHECK(std::get<MailTransferExecutionSummary>(result).status == MailTransferStatus::Complete);
    CHECK(fixture.methodTransport.existingGetCalls == 1);
    CHECK(fixture.methodTransport.setCalls == 1);
    CHECK(fixture.methodTransport.setArguments.find("mailboxIds/archive") != std::string::npos);
    CHECK(fixture.methodTransport.setArguments.find("keywords") == std::string::npos);
    CHECK(fixture.methodTransport.setArguments.find(
              "\"ifInState\":\"destination-state-existing\"") != std::string::npos);
    const auto item = fixture.item(operationId);
    CHECK(item.phase == MailTransferItemPhase::Complete);
    CHECK(item.reusedExisting);
    CHECK(item.destinationEmailId == std::optional<std::string>{"existing-email"});
    CHECK(item.destinationPriorMailboxIds ==
          std::optional<std::vector<std::string>>{{"old-mailbox"}});
    CHECK(fixture.pinCount() == 0);
    CHECK(fixture.sourceStillExists());
}

TEST_CASE("lost duplicate mailbox update response keeps durable reconciliation evidence",
          "[app][mail-transfer][executor][duplicate][ambiguity]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    fixture.methodTransport.behavior = ImportBehavior::DuplicateMembershipDispatchedFailure;
    const auto operationId = fixture.prepare(MailTransferOperation::Move);

    const auto first = fixture.execute(operationId);
    REQUIRE(std::holds_alternative<MailTransferExecutionSummary>(first));
    CHECK(std::get<MailTransferExecutionSummary>(first).status ==
          MailTransferStatus::BlockedUnknown);
    const auto item = fixture.item(operationId);
    CHECK(item.phase == MailTransferItemPhase::DestinationUnknown);
    CHECK(item.reusedExisting);
    CHECK(item.destinationEmailId == std::optional<std::string>{"existing-email"});
    CHECK(item.destinationPreState ==
          std::optional<std::string>{"destination-state-existing"});
    CHECK(item.destinationPriorMailboxIds ==
          std::optional<std::vector<std::string>>{{"old-mailbox"}});
    CHECK(fixture.pinCount() == 1);
    CHECK(fixture.sourceStillExists());
    const int setCount = fixture.methodTransport.setCalls;
    const int importCount = fixture.methodTransport.importCalls;

    fixture.methodTransport.behavior = ImportBehavior::Success;
    const auto second = fixture.execute(operationId);
    REQUIRE(std::holds_alternative<MailTransferExecutionSummary>(second));
    CHECK(std::get<MailTransferExecutionSummary>(second).status ==
          MailTransferStatus::BlockedUnknown);
    CHECK(fixture.methodTransport.setCalls == setCount);
    CHECK(fixture.methodTransport.importCalls == importCount);
    CHECK(fixture.sourceStillExists());
}
