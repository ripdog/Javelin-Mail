#include "app/MailTransferApplicationService.h"
#include "app/MailTransferExecutor.h"
#include "app/MailTransferRepository.h"
#include "app/AccountConnectionProvider.h"
#include "app/undo/HistoryRepository.h"
#include "app/undo/MailTransferHistoryCoordinator.h"
#include "app/undo/MailTransferHistoryService.h"
#include "app/undo/UndoManager.h"
#include "jmap/MessageContentClient.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/Session.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/RawMessageSourceRepository.h"
#include "jmap/cache/SearchWindowRepository.h"
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

    enum class SourceCleanupBehavior
    {
        Success,
        Reject,
        DispatchedFailure,
        NotFound,
        ConcurrentNewMailbox,
    };

    class RecordingMethodTransport final : public javelin::jmap::api::JmapMethodTransport
    {
      public:
        ImportBehavior behavior = ImportBehavior::Success;
        SourceCleanupBehavior sourceCleanupBehavior = SourceCleanupBehavior::Success;
        int stateCalls = 0;
        int existingGetCalls = 0;
        int materializeCalls = 0;
        int changesCalls = 0;
        int candidateGetCalls = 0;
        int importCalls = 0;
        int setCalls = 0;
        int sourceGetCalls = 0;
        int sourceSetCalls = 0;
        std::string importArguments;
        std::string setArguments;
        std::string sourceSetArguments;
        std::string existingState{"destination-state-existing"};
        bool existingMembershipApplied = false;
        bool materializeNotFound = false;
        bool reconciliationCandidateMatchesSource = false;
        std::vector<std::string> reconciliationCreatedIds;
        std::vector<std::string> sourceAuthoritativeMailboxIds{"inbox"};
        std::vector<std::string> callOrder;

        [[nodiscard]] QCoro::Task<JmapMethodTransportResult>
        call(JmapMethodRequest request) override
        {
            if (request.apiUrl.find("source.example.test") != std::string::npos)
            {
                REQUIRE_FALSE(request.envelope.methodCalls.empty());
                const auto& sourceMethod = request.envelope.methodCalls.front();
                if (sourceMethod.name == "Email/get")
                {
                    REQUIRE(request.envelope.methodCalls.size() == 1);
                    ++sourceGetCalls;
                    callOrder.push_back("source-get");
                    if (request.dispatched)
                        request.dispatched();
                    if (sourceCleanupBehavior == SourceCleanupBehavior::NotFound)
                    {
                        co_return ResponseEnvelope{
                            .methodResponses = {{
                                .name = "Email/get",
                                .arguments =
                                    R"({"accountId":"u1","state":"source-state-current","list":[],"notFound":["email-1"]})",
                                .callId = sourceMethod.callId,
                            }},
                            .createdIds = std::nullopt,
                            .sessionState = "session-state",
                        };
                    }

                    auto mailboxIds = sourceAuthoritativeMailboxIds;
                    if (sourceCleanupBehavior == SourceCleanupBehavior::ConcurrentNewMailbox)
                        mailboxIds = {"inbox", "new-mailbox"};
                    std::string mailboxJson{"{"};
                    for (const auto& mailboxId : mailboxIds)
                    {
                        if (mailboxJson.size() > 1)
                            mailboxJson += ',';
                        mailboxJson += '"' + mailboxId + R"(":true)";
                    }
                    mailboxJson += '}';
                    const auto response =
                        std::string{R"({"accountId":"u1","state":"source-state-current","list":[{"id":"email-1","blobId":"blob-1","threadId":"thread-1","mailboxIds":)"} +
                        mailboxJson +
                        R"(,"keywords":{"$seen":true,"$flagged":true},"size":2048,"receivedAt":"2026-08-15T08:00:00Z","messageId":["mail-transfer@example.test"],"inReplyTo":[],"references":[],"hasAttachment":false,"subject":"Transfer","from":[],"to":[],"cc":[],"bcc":[],"replyTo":[],"preview":"Preview"}],"notFound":[]})";
                    co_return ResponseEnvelope{
                        .methodResponses = {{.name = "Email/get",
                                             .arguments = response,
                                             .callId = sourceMethod.callId}},
                        .createdIds = std::nullopt,
                        .sessionState = "session-state",
                    };
                }

                REQUIRE(sourceMethod.name == "Email/set");
                ++sourceSetCalls;
                callOrder.push_back("source-set");
                sourceSetArguments = sourceMethod.arguments;
                if (request.dispatched)
                    request.dispatched();
                if (sourceCleanupBehavior == SourceCleanupBehavior::DispatchedFailure)
                {
                    co_return TransportError{
                        .code = TransportErrorCode::NetworkFailure,
                        .message = "connection lost after source cleanup dispatch",
                        .httpStatus = std::nullopt,
                        .networkError = std::nullopt,
                        .retryAfter = std::nullopt,
                    };
                }

                const bool destroys = sourceMethod.arguments.find("\"destroy\":[\"email-1\"]") !=
                                      std::string::npos;
                std::string sourceResponseArguments;
                if (sourceCleanupBehavior == SourceCleanupBehavior::Reject)
                {
                    sourceResponseArguments = destroys
                                       ? R"({"accountId":"u1","oldState":"source-state-current","newState":"source-state-current","created":{},"updated":{},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{"email-1":{"type":"forbidden","description":"Cleanup denied","properties":[]}}})"
                                       : R"({"accountId":"u1","oldState":"source-state-current","newState":"source-state-current","created":{},"updated":{},"destroyed":[],"notCreated":{},"notUpdated":{"email-1":{"type":"forbidden","description":"Cleanup denied","properties":[]}},"notDestroyed":{}})";
                }
                else
                {
                    sourceResponseArguments = destroys
                                       ? R"({"accountId":"u1","oldState":"source-state-current","newState":"source-state-next","created":{},"updated":{},"destroyed":["email-1"],"notCreated":{},"notUpdated":{},"notDestroyed":{}})"
                                       : R"({"accountId":"u1","oldState":"source-state-current","newState":"source-state-next","created":{},"updated":{"email-1":null},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{}})";
                }

                std::vector<javelin::jmap::api::MethodInvocation> responses;
                responses.push_back({
                    .name = "Email/set",
                    .arguments = std::move(sourceResponseArguments),
                    .callId = sourceMethod.callId,
                });
                for (std::size_t index = 1; index < request.envelope.methodCalls.size(); ++index)
                {
                    const auto& extra = request.envelope.methodCalls[index];
                    REQUIRE(extra.name == "Mailbox/get");
                    responses.push_back({
                        .name = "error",
                        .arguments = R"({"type":"serverUnavailable"})",
                        .callId = extra.callId,
                    });
                }
                co_return ResponseEnvelope{
                    .methodResponses = std::move(responses),
                    .createdIds = std::nullopt,
                    .sessionState = "session-state",
                };
            }

            REQUIRE(request.envelope.methodCalls.size() == 1);
            const auto& method = request.envelope.methodCalls.front();
            if (method.name == "Email/get")
            {
                if (request.dispatched)
                    request.dispatched();
                if (method.arguments.find("existing-email") != std::string::npos)
                {
                    ++existingGetCalls;
                    callOrder.push_back("destination-existing-get");
                    const bool inTarget = behavior == ImportBehavior::AlreadyExistsInTarget ||
                                          existingMembershipApplied;
                    const auto mailboxIds =
                        inTarget ? R"({"archive":true,"old-mailbox":true})"
                                 : R"({"old-mailbox":true})";
                    const auto response =
                        std::string{R"({"accountId":"u1","state":")"} + existingState +
                        R"(","list":[{"id":"existing-email","blobId":"existing-blob","threadId":"existing-thread","mailboxIds":)" +
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

                if (method.arguments.find("destination-email") != std::string::npos)
                {
                    ++materializeCalls;
                    callOrder.push_back("destination-materialize");
                    if (materializeNotFound)
                    {
                        co_return ResponseEnvelope{
                            .methodResponses = {{
                                .name = "Email/get",
                                .arguments =
                                    R"({"accountId":"u1","state":"destination-state-3","list":[],"notFound":["destination-email"]})",
                                .callId = method.callId,
                            }},
                            .createdIds = std::nullopt,
                            .sessionState = "session-state",
                        };
                    }
                    co_return ResponseEnvelope{
                        .methodResponses = {{
                            .name = "Email/get",
                            .arguments =
                                R"({"accountId":"u1","state":"destination-state-2","list":[{"id":"destination-email","blobId":"destination-blob","threadId":"destination-thread","mailboxIds":{"archive":true},"keywords":{"$seen":true,"$flagged":true},"size":37,"receivedAt":"2026-08-15T08:00:00Z","hasAttachment":false,"subject":"Transfer","from":[],"to":[],"cc":[],"bcc":[],"replyTo":[],"preview":"Preview"}],"notFound":[]})",
                            .callId = method.callId,
                        }},
                        .createdIds = std::nullopt,
                        .sessionState = "session-state",
                    };
                }

                bool candidateRequest = false;
                for (const auto& candidateId : reconciliationCreatedIds)
                {
                    candidateRequest = candidateRequest ||
                                       method.arguments.find(candidateId) != std::string::npos;
                }
                if (candidateRequest)
                {
                    ++candidateGetCalls;
                    std::string list;
                    for (const auto& candidateId : reconciliationCreatedIds)
                    {
                        if (method.arguments.find(candidateId) == std::string::npos)
                            continue;
                        if (!list.empty())
                            list += ',';
                        const bool matches = reconciliationCandidateMatchesSource &&
                                             candidateId == reconciliationCreatedIds.front();
                        list += std::string{R"({"id":")"} + candidateId +
                                R"(","blobId":"candidate-blob","threadId":"candidate-thread","mailboxIds":{"archive":true},"keywords":{"$seen":true},"size":)" +
                                (matches ? "2048" : "999") +
                                R"(,"receivedAt":")" +
                                (matches ? "2026-08-15T08:00:00Z" : "2026-08-10T00:00:00Z") +
                                R"(","messageId":[")" +
                                (matches ? "mail-transfer@example.test" : "unrelated@example.test") +
                                R"("],"hasAttachment":false,"subject":"Candidate","from":[],"to":[],"cc":[],"bcc":[],"replyTo":[],"preview":"Candidate"})";
                    }
                    co_return ResponseEnvelope{
                        .methodResponses = {{
                            .name = "Email/get",
                            .arguments = std::string{R"({"accountId":"u1","state":"destination-state-reconciled","list":[)"} +
                                         list + R"(],"notFound":[]})",
                            .callId = method.callId,
                        }},
                        .createdIds = std::nullopt,
                        .sessionState = "session-state",
                    };
                }

                ++stateCalls;
                callOrder.push_back("destination-state-get");
                co_return ResponseEnvelope{
                    .methodResponses = {{.name = "Email/get",
                                         .arguments =
                                             R"({"accountId":"u1","state":"destination-state-1","list":[],"notFound":[]})",
                                         .callId = method.callId}},
                    .createdIds = std::nullopt,
                    .sessionState = "session-state",
                };
            }

            if (method.name == "Email/changes")
            {
                ++changesCalls;
                if (request.dispatched)
                    request.dispatched();
                const auto arguments =
                    QJsonDocument::fromJson(QByteArray::fromStdString(method.arguments)).object();
                const auto sinceState = arguments.value(QStringLiteral("sinceState")).toString();
                std::string created;
                for (const auto& id : reconciliationCreatedIds)
                {
                    if (!created.empty())
                        created += ',';
                    created += '"' + id + '"';
                }
                co_return ResponseEnvelope{
                    .methodResponses = {{
                        .name = "Email/changes",
                        .arguments = QStringLiteral(
                                         R"({"accountId":"u1","oldState":"%1","newState":"destination-state-reconciled","hasMoreChanges":false,"created":[%2],"updated":[],"destroyed":[]})")
                                         .arg(sinceState,
                                              QString::fromStdString(std::move(created)))
                                         .toStdString(),
                        .callId = method.callId,
                    }},
                    .createdIds = std::nullopt,
                    .sessionState = "session-state",
                };
            }

            if (method.name == "Email/set")
            {
                ++setCalls;
                callOrder.push_back("destination-set");
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
                existingMembershipApplied = true;
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
            callOrder.push_back("destination-import");
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

        [[nodiscard]] QString createTransferHistoryEntry(QString entryId)
        {
            javelin::app::undo::HistoryRepository repository{database};
            javelin::app::undo::HistoryEntry entry;
            entry.entryId = entryId;
            entry.stack = javelin::app::undo::HistoryStack::Redo;
            entry.label = QStringLiteral("Redo transfer");
            entry.domain = javelin::app::undo::HistoryDomain::Mail;
            entry.commandKind = QStringLiteral("mail_transfer");
            entry.payloadVersion = 1;
            entry.payload = javelin::app::undo::MailTransferHistory{
                .sourceAccountId = sourceAccountId,
                .destinationAccountId = destinationAccountId,
                .destinationMailboxId = "archive",
                .operation = javelin::app::undo::MailTransferHistoryOperation::Move,
                .items = {},
            };
            entry.status = javelin::app::undo::HistoryEntryStatus::Ready;
            const auto pushed = repository.pushUndoClearingRedo(std::move(entry));
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&pushed))
                FAIL(error->message.toStdString());
            return entryId;
        }

        void setSourceMailboxIds(std::vector<std::string> mailboxIds)
        {
            javelin::jmap::cache::EmailRepository emails{database};
            const auto found = emails.find(sourceAccountId, "email-1");
            REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(found));
            auto current = std::get<std::optional<javelin::jmap::domain::Email>>(found);
            REQUIRE(current.has_value());
            current->mailboxIds = mailboxIds;
            REQUIRE_FALSE(emails.upsertMany(sourceAccountId, {*current}).has_value());

            std::vector<javelin::jmap::domain::Mailbox> sourceMailboxes;
            sourceMailboxes.reserve(mailboxIds.size());
            for (const auto& mailboxId : mailboxIds)
                sourceMailboxes.push_back(mailbox(mailboxId, mailboxId));
            javelin::jmap::cache::MailboxRepository mailboxes{database};
            REQUIRE_FALSE(mailboxes.replaceAll(sourceAccountId, sourceMailboxes).has_value());
            methodTransport.sourceAuthoritativeMailboxIds = std::move(mailboxIds);
        }

        [[nodiscard]] std::optional<std::vector<std::string>> sourceMailboxIds() const
        {
            javelin::jmap::cache::EmailRepository emails{
                const_cast<javelin::jmap::cache::DatabaseConnection&>(database)};
            const auto result = emails.find(sourceAccountId, "email-1");
            REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(result));
            const auto& current = std::get<std::optional<javelin::jmap::domain::Email>>(result);
            return current.has_value() ? std::optional{current->mailboxIds} : std::nullopt;
        }

        [[nodiscard]] std::optional<javelin::jmap::domain::Email>
        destinationEmail(std::string_view emailId) const
        {
            javelin::jmap::cache::EmailRepository emails{
                const_cast<javelin::jmap::cache::DatabaseConnection&>(database)};
            const auto result = emails.find(destinationAccountId, emailId);
            REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(result));
            return std::get<std::optional<javelin::jmap::domain::Email>>(result);
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

    class SameSessionMethodTransport final : public javelin::jmap::api::JmapMethodTransport
    {
      public:
        bool copyDispatchedFailure = false;
        bool reconciliationCandidateMatchesSource = false;
        std::vector<std::string> reconciliationCreatedIds;
        std::vector<std::string> sourceAuthoritativeMailboxIds{"inbox"};
        int copyCalls = 0;
        int materializeCalls = 0;
        int changesCalls = 0;
        int candidateGetCalls = 0;
        int sourceGetCalls = 0;
        int sourceSetCalls = 0;
        std::string copyArguments;
        std::string sourceSetArguments;

        [[nodiscard]] QCoro::Task<JmapMethodTransportResult>
        call(JmapMethodRequest request) override
        {
            REQUIRE(request.apiUrl == "https://shared.example.test/jmap");
            REQUIRE_FALSE(request.envelope.methodCalls.empty());
            const auto& method = request.envelope.methodCalls.front();
            if (method.name == "Email/get")
            {
                REQUIRE(request.envelope.methodCalls.size() == 1);
                if (request.dispatched)
                    request.dispatched();
                if (method.arguments.find("destination-email") != std::string::npos)
                {
                    ++materializeCalls;
                    co_return ResponseEnvelope{
                        .methodResponses = {{
                            .name = "Email/get",
                            .arguments =
                                R"({"accountId":"u2","state":"destination-state-2","list":[{"id":"destination-email","blobId":"destination-blob","threadId":"destination-thread","mailboxIds":{"archive":true},"keywords":{"$seen":true,"$flagged":true},"size":41,"receivedAt":"2026-08-15T08:00:00Z","hasAttachment":false,"subject":"Transfer","from":[],"to":[],"cc":[],"bcc":[],"replyTo":[],"preview":"Preview"}],"notFound":[]})",
                            .callId = method.callId,
                        }},
                        .createdIds = std::nullopt,
                        .sessionState = "session-state",
                    };
                }
                bool candidateRequest = false;
                for (const auto& candidateId : reconciliationCreatedIds)
                    candidateRequest = candidateRequest ||
                                       method.arguments.find(candidateId) != std::string::npos;
                if (candidateRequest)
                {
                    ++candidateGetCalls;
                    std::string list;
                    for (const auto& candidateId : reconciliationCreatedIds)
                    {
                        if (method.arguments.find(candidateId) == std::string::npos)
                            continue;
                        if (!list.empty())
                            list += ',';
                        const bool matches = reconciliationCandidateMatchesSource &&
                                             candidateId == reconciliationCreatedIds.front();
                        list += std::string{R"({"id":")"} + candidateId +
                                R"(","blobId":"candidate-blob","threadId":"candidate-thread","mailboxIds":{"archive":true},"keywords":{"$seen":true},"size":)" +
                                (matches ? "2048" : "999") +
                                R"(,"receivedAt":")" +
                                (matches ? "2026-08-15T08:00:00Z" : "2026-08-10T00:00:00Z") +
                                R"(","messageId":[")" +
                                (matches ? "mail-transfer@example.test" : "unrelated@example.test") +
                                R"("],"hasAttachment":false,"subject":"Candidate","from":[],"to":[],"cc":[],"bcc":[],"replyTo":[],"preview":"Candidate"})";
                    }
                    co_return ResponseEnvelope{
                        .methodResponses = {{
                            .name = "Email/get",
                            .arguments = std::string{R"({"accountId":"u2","state":"destination-state-reconciled","list":[)"} +
                                         list + R"(],"notFound":[]})",
                            .callId = method.callId,
                        }},
                        .createdIds = std::nullopt,
                        .sessionState = "session-state",
                    };
                }
                if (method.arguments.find("email-1") == std::string::npos)
                {
                    co_return ResponseEnvelope{
                        .methodResponses = {{
                            .name = "Email/get",
                            .arguments =
                                R"({"accountId":"u2","state":"destination-state-1","list":[],"notFound":[]})",
                            .callId = method.callId,
                        }},
                        .createdIds = std::nullopt,
                        .sessionState = "session-state",
                    };
                }

                ++sourceGetCalls;
                std::string mailboxJson{"{"};
                for (const auto& mailboxId : sourceAuthoritativeMailboxIds)
                {
                    if (mailboxJson.size() > 1)
                        mailboxJson += ',';
                    mailboxJson += '"' + mailboxId + R"(":true)";
                }
                mailboxJson += '}';
                const auto response =
                    std::string{R"({"accountId":"u1","state":"source-state-current","list":[{"id":"email-1","mailboxIds":)"} +
                    mailboxJson +
                    R"(,"keywords":{"$seen":true,"$flagged":true},"subject":"Transfer"}],"notFound":[]})";
                co_return ResponseEnvelope{
                    .methodResponses = {{.name = "Email/get",
                                         .arguments = response,
                                         .callId = method.callId}},
                    .createdIds = std::nullopt,
                    .sessionState = "session-state",
                };
            }

            if (method.name == "Email/changes")
            {
                ++changesCalls;
                if (request.dispatched)
                    request.dispatched();
                const auto arguments =
                    QJsonDocument::fromJson(QByteArray::fromStdString(method.arguments)).object();
                const auto sinceState = arguments.value(QStringLiteral("sinceState")).toString();
                std::string created;
                for (const auto& id : reconciliationCreatedIds)
                {
                    if (!created.empty())
                        created += ',';
                    created += '"' + id + '"';
                }
                co_return ResponseEnvelope{
                    .methodResponses = {{
                        .name = "Email/changes",
                        .arguments = QStringLiteral(
                                         R"({"accountId":"u2","oldState":"%1","newState":"destination-state-reconciled","hasMoreChanges":false,"created":[%2],"updated":[],"destroyed":[]})")
                                         .arg(sinceState,
                                              QString::fromStdString(std::move(created)))
                                         .toStdString(),
                        .callId = method.callId,
                    }},
                    .createdIds = std::nullopt,
                    .sessionState = "session-state",
                };
            }

            if (method.name == "Email/copy")
            {
                REQUIRE(request.envelope.methodCalls.size() == 1);
                ++copyCalls;
                copyArguments = method.arguments;
                if (request.dispatched)
                    request.dispatched();
                if (copyDispatchedFailure)
                {
                    co_return TransportError{
                        .code = TransportErrorCode::NetworkFailure,
                        .message = "connection lost after Email/copy dispatch",
                        .httpStatus = std::nullopt,
                        .networkError = std::nullopt,
                        .retryAfter = std::nullopt,
                    };
                }
                const auto arguments =
                    QJsonDocument::fromJson(QByteArray::fromStdString(method.arguments)).object();
                const auto create = arguments.value(QStringLiteral("create")).toObject();
                REQUIRE(create.size() == 1);
                const auto creationId = create.begin().key();
                const auto response = QStringLiteral(
                                          R"({"fromAccountId":"u1","accountId":"u2","oldState":"destination-state-1","newState":"destination-state-2","created":{"%1":{"id":"destination-email","blobId":"destination-blob","threadId":"destination-thread","size":41}},"notCreated":{}})")
                                          .arg(creationId)
                                          .toStdString();
                co_return ResponseEnvelope{
                    .methodResponses = {{.name = "Email/copy",
                                         .arguments = response,
                                         .callId = method.callId}},
                    .createdIds = std::nullopt,
                    .sessionState = "session-state",
                };
            }

            REQUIRE(method.name == "Email/set");
            ++sourceSetCalls;
            sourceSetArguments = method.arguments;
            if (request.dispatched)
                request.dispatched();
            const bool destroys =
                method.arguments.find("\"destroy\":[\"email-1\"]") != std::string::npos;
            const std::string response =
                destroys
                    ? R"({"accountId":"u1","oldState":"source-state-current","newState":"source-state-next","created":{},"updated":{},"destroyed":["email-1"],"notCreated":{},"notUpdated":{},"notDestroyed":{}})"
                    : R"({"accountId":"u1","oldState":"source-state-current","newState":"source-state-next","created":{},"updated":{"email-1":null},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{}})";
            std::vector<javelin::jmap::api::MethodInvocation> responses{{
                .name = "Email/set",
                .arguments = response,
                .callId = method.callId,
            }};
            for (std::size_t index = 1; index < request.envelope.methodCalls.size(); ++index)
            {
                const auto& extra = request.envelope.methodCalls[index];
                REQUIRE(extra.name == "Mailbox/get");
                responses.push_back({
                    .name = "error",
                    .arguments = R"({"type":"serverUnavailable"})",
                    .callId = extra.callId,
                });
            }
            co_return ResponseEnvelope{
                .methodResponses = std::move(responses),
                .createdIds = std::nullopt,
                .sessionState = "session-state",
            };
        }
    };

    struct SameSessionFixture
    {
        QTemporaryDir directory;
        javelin::jmap::cache::DatabaseConnection database;
        std::string sourceAccountId;
        std::string destinationAccountId;
        ConnectionProvider connections;
        RecordingResourceTransport resourceTransport;
        SameSessionMethodTransport methodTransport;
        std::unique_ptr<javelin::jmap::MessageContentClient> contentClient;
        QByteArray raw = QByteArrayLiteral("From: sender@example.test\r\nSubject: Shared\r\n\r\nBody\r\n");

        SameSessionFixture()
            : database(openDatabase([this]
                                    {
                                        REQUIRE(directory.isValid());
                                        return directory.filePath(QStringLiteral("shared.sqlite3"));
                                    }()))
        {
            auto shared = session("https://shared.example.test/jmap",
                                  "https://shared.example.test/upload/{accountId}");
            shared.accounts.emplace(
                "u2", javelin::jmap::api::Account{
                          .id = "u2",
                          .name = "Destination",
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
            javelin::jmap::cache::SessionRepository sessions{database};
            const auto stored = sessions.replaceForConnection("connection-shared", "u1", shared);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&stored))
                FAIL(error->message.toStdString());
            const auto& mapping =
                std::get<javelin::jmap::cache::StoredSessionAccounts>(stored).accountIdsByRemoteId;
            sourceAccountId = mapping.at("u1");
            destinationAccountId = mapping.at("u2");
            REQUIRE(sourceAccountId != destinationAccountId);

            const AccountConnectionSettings settings{
                .connectionId = "connection-shared",
                .revision = 1,
                .displayName = "Shared",
                .sessionUrl = "https://shared.example.test/session",
                .loginEmail = "shared@example.test",
                .apiKey = "shared-token",
                .refreshToken = {},
                .tokenEndpoint = {},
                .oauthClientId = {},
                .oauthIssuer = {},
                .oauthResource = {},
                .oauthScope = {},
                .revocationEndpoint = {},
                .registrationClientUri = {},
                .registrationAccessToken = {},
            };
            connections.settings.emplace(sourceAccountId, settings);
            connections.settings.emplace(destinationAccountId, settings);

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
            contentClient = std::make_unique<javelin::jmap::MessageContentClient>(database,
                                                                                  resourceTransport);
        }

        void seedRawSource()
        {
            javelin::jmap::cache::RawMessageSourceRepository sources{database};
            REQUIRE_FALSE(sources
                              .upsert(sourceAccountId,
                                      {.emailId = "email-1", .blobId = "blob-1", .payload = raw})
                              .has_value());
        }

        void setSourceMailboxIds(std::vector<std::string> mailboxIds)
        {
            javelin::jmap::cache::EmailRepository emails{database};
            const auto found = emails.find(sourceAccountId, "email-1");
            REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(found));
            auto current = std::get<std::optional<javelin::jmap::domain::Email>>(found);
            REQUIRE(current.has_value());
            current->mailboxIds = mailboxIds;
            REQUIRE_FALSE(emails.upsertMany(sourceAccountId, {*current}).has_value());
            std::vector<javelin::jmap::domain::Mailbox> sourceMailboxes;
            for (const auto& mailboxId : mailboxIds)
                sourceMailboxes.push_back(mailbox(mailboxId, mailboxId));
            javelin::jmap::cache::MailboxRepository mailboxes{database};
            REQUIRE_FALSE(mailboxes.replaceAll(sourceAccountId, sourceMailboxes).has_value());
            methodTransport.sourceAuthoritativeMailboxIds = std::move(mailboxIds);
        }

        [[nodiscard]] std::string prepare(const MailTransferOperation operation)
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
            CHECK(std::get<PreparedMailTransfer>(prepared).topology ==
                  MailTransferTopology::SameSessionCopy);
            return std::get<PreparedMailTransfer>(prepared).operationId;
        }

        [[nodiscard]] MailTransferExecutionResult execute(const std::string& operationId)
        {
            MailTransferExecutor executor{database, resourceTransport, methodTransport,
                                          *contentClient, connections};
            return QCoro::waitFor(executor.advance(operationId));
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

        [[nodiscard]] std::optional<std::vector<std::string>> sourceMailboxIds()
        {
            javelin::jmap::cache::EmailRepository emails{database};
            const auto result = emails.find(sourceAccountId, "email-1");
            REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(result));
            const auto& current = std::get<std::optional<javelin::jmap::domain::Email>>(result);
            return current.has_value() ? std::optional{current->mailboxIds} : std::nullopt;
        }

        [[nodiscard]] int pinCount()
        {
            QSqlQuery query{database.database()};
            REQUIRE(query.exec(QStringLiteral("SELECT COUNT(*) FROM mail_vault_pins")));
            REQUIRE(query.next());
            return query.value(0).toInt();
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
    javelin::jmap::cache::MailboxWindowRepository mailboxWindows{fixture.database};
    REQUIRE_FALSE(mailboxWindows
                      .replace({.accountId = fixture.destinationAccountId,
                                .mailboxId = "archive",
                                .queryKey = "archive-window",
                                .requestedOffset = 0,
                                .requestedLimit = 50,
                                .position = 0,
                                .returnedLimit = 0,
                                .total = 0,
                                .queryState = "destination-query-state",
                                .coverage = javelin::jmap::cache::QueryWindowCoverage::Server,
                                .materialization =
                                    javelin::jmap::cache::QueryWindowMaterialization::Complete,
                                .emailIds = {}})
                      .has_value());
    javelin::jmap::cache::SearchWindowRepository searchWindows{fixture.database};
    REQUIRE_FALSE(searchWindows
                      .replace({.accountId = fixture.destinationAccountId,
                                .queryKey = "search-window",
                                .offset = 0,
                                .limit = 50,
                                .position = 0,
                                .returnedLimit = 0,
                                .total = 0,
                                .queryState = "destination-search-state",
                                .coverage = javelin::jmap::cache::QueryWindowCoverage::Server,
                                .materialization =
                                    javelin::jmap::cache::QueryWindowMaterialization::Complete,
                                .emailIds = {}})
                      .has_value());

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
    CHECK(fixture.methodTransport.materializeCalls == 1);
    const auto cachedDestination = fixture.destinationEmail("destination-email");
    REQUIRE(cachedDestination.has_value());
    CHECK(cachedDestination->threadId == "destination-thread");
    CHECK(cachedDestination->mailboxIds == std::vector<std::string>{"archive"});
    CHECK(cachedDestination->keywords == std::vector<std::string>{"$flagged", "$seen"});
    const auto mailboxWindow =
        mailboxWindows.find(fixture.destinationAccountId, "archive-window", 0, 50);
    REQUIRE(std::holds_alternative<
            std::optional<javelin::jmap::cache::MailboxWindowRecord>>(mailboxWindow));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(mailboxWindow)
                .has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(mailboxWindow)
              ->coverage == javelin::jmap::cache::QueryWindowCoverage::Stale);
    const auto searchWindow =
        searchWindows.find(fixture.destinationAccountId, "search-window", 0, 50);
    REQUIRE(std::holds_alternative<
            std::optional<javelin::jmap::cache::SearchWindowRecord>>(searchWindow));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::SearchWindowRecord>>(searchWindow)
                .has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::SearchWindowRecord>>(searchWindow)->coverage ==
          javelin::jmap::cache::QueryWindowCoverage::Stale);
    CHECK(fixture.pinCount() == 0);
    CHECK(fixture.sourceStillExists());
}

TEST_CASE("cross-server move destroys the last source residency only after destination confirmation",
          "[app][mail-transfer][executor][move]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    const auto operationId = fixture.prepare(MailTransferOperation::Move);

    const auto result = fixture.execute(operationId);
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        FAIL(error->message.toStdString());
    const auto& summary = std::get<MailTransferExecutionSummary>(result);
    CHECK(summary.status == MailTransferStatus::Complete);
    CHECK(summary.completeItemCount == 1);
    const auto item = fixture.item(operationId);
    CHECK(item.phase == MailTransferItemPhase::Complete);
    CHECK(item.sourceDestroy);
    CHECK(item.sourceRemoveMailboxIds == std::vector<std::string>{"inbox"});
    CHECK(item.sourceEmailState == std::optional<std::string>{"source-state-current"});
    CHECK(fixture.methodTransport.sourceGetCalls == 1);
    CHECK(fixture.methodTransport.sourceSetCalls == 1);
    CHECK(fixture.methodTransport.sourceSetArguments.find("\"destroy\":[\"email-1\"]") !=
          std::string::npos);
    const auto importPosition = std::ranges::find(fixture.methodTransport.callOrder,
                                                  std::string{"destination-import"});
    const auto sourceGetPosition = std::ranges::find(fixture.methodTransport.callOrder,
                                                     std::string{"source-get"});
    const auto sourceSetPosition = std::ranges::find(fixture.methodTransport.callOrder,
                                                     std::string{"source-set"});
    REQUIRE(importPosition != fixture.methodTransport.callOrder.end());
    REQUIRE(sourceGetPosition != fixture.methodTransport.callOrder.end());
    REQUIRE(sourceSetPosition != fixture.methodTransport.callOrder.end());
    CHECK(importPosition < sourceGetPosition);
    CHECK(sourceGetPosition < sourceSetPosition);
    CHECK(fixture.pinCount() == 1);
    CHECK_FALSE(fixture.sourceStillExists());
}

TEST_CASE("move keeps source when confirmed destination cannot be materialized",
          "[app][mail-transfer][executor][move][destination-cache]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    fixture.methodTransport.materializeNotFound = true;
    const auto operationId = fixture.prepare(MailTransferOperation::Move);

    const auto result = fixture.execute(operationId);
    REQUIRE(std::holds_alternative<MailTransferExecutionSummary>(result));
    CHECK(std::get<MailTransferExecutionSummary>(result).status ==
          MailTransferStatus::BlockedUnknown);
    const auto item = fixture.item(operationId);
    CHECK(item.phase == MailTransferItemPhase::DestinationUnknown);
    CHECK(item.destinationEmailId == std::optional<std::string>{"destination-email"});
    CHECK(fixture.methodTransport.materializeCalls == 1);
    CHECK(fixture.methodTransport.sourceGetCalls == 0);
    CHECK(fixture.methodTransport.sourceSetCalls == 0);
    CHECK(fixture.sourceStillExists());
    CHECK_FALSE(fixture.destinationEmail("destination-email").has_value());
    CHECK(fixture.pinCount() == 1);
}

TEST_CASE("cross-server move removes only the selected source mailbox when other residency remains",
          "[app][mail-transfer][executor][move]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    fixture.setSourceMailboxIds({"inbox", "important"});
    const auto operationId = fixture.prepare(MailTransferOperation::Move);

    const auto result = fixture.execute(operationId);
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        FAIL(error->message.toStdString());
    CHECK(std::get<MailTransferExecutionSummary>(result).status == MailTransferStatus::Complete);
    const auto item = fixture.item(operationId);
    CHECK(item.phase == MailTransferItemPhase::Complete);
    CHECK_FALSE(item.sourceDestroy);
    CHECK(item.sourceRemoveMailboxIds == std::vector<std::string>{"inbox"});
    CHECK(fixture.methodTransport.sourceSetCalls == 1);
    CHECK(fixture.methodTransport.sourceSetArguments.find("mailboxIds/inbox") !=
          std::string::npos);
    CHECK(fixture.methodTransport.sourceSetArguments.find("\"destroy\"") == std::string::npos);
    REQUIRE(fixture.sourceMailboxIds().has_value());
    CHECK(*fixture.sourceMailboxIds() == std::vector<std::string>{"important"});
    CHECK(fixture.pinCount() == 0);
}

TEST_CASE("concurrent new source residency downgrades planned destroy to mailbox removal",
          "[app][mail-transfer][executor][move][concurrency]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    const auto operationId = fixture.prepare(MailTransferOperation::Move);
    REQUIRE(fixture.item(operationId).sourceDestroy);
    fixture.methodTransport.sourceCleanupBehavior = SourceCleanupBehavior::ConcurrentNewMailbox;

    const auto result = fixture.execute(operationId);
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        FAIL(error->message.toStdString());
    CHECK(std::get<MailTransferExecutionSummary>(result).status == MailTransferStatus::Complete);
    const auto item = fixture.item(operationId);
    CHECK(item.phase == MailTransferItemPhase::Complete);
    CHECK_FALSE(item.sourceDestroy);
    CHECK(item.sourceRemoveMailboxIds == std::vector<std::string>{"inbox"});
    CHECK(fixture.methodTransport.sourceSetArguments.find("mailboxIds/inbox") !=
          std::string::npos);
    CHECK(fixture.methodTransport.sourceSetArguments.find("\"destroy\"") == std::string::npos);
    REQUIRE(fixture.sourceMailboxIds().has_value());
    CHECK(*fixture.sourceMailboxIds() == std::vector<std::string>{"new-mailbox"});
    CHECK(fixture.pinCount() == 0);
}

TEST_CASE("definitive source cleanup rejection keeps confirmed destination and reports partial move",
          "[app][mail-transfer][executor][move][source-rejection]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    fixture.methodTransport.sourceCleanupBehavior = SourceCleanupBehavior::Reject;
    const auto operationId = fixture.prepare(MailTransferOperation::Move);

    const auto result = fixture.execute(operationId);
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        FAIL(error->message.toStdString());
    const auto& summary = std::get<MailTransferExecutionSummary>(result);
    CHECK(summary.status == MailTransferStatus::Partial);
    CHECK(summary.partialItemCount == 1);
    const auto item = fixture.item(operationId);
    CHECK(item.phase == MailTransferItemPhase::PartialSourceRetained);
    CHECK(item.destinationEmailId == std::optional<std::string>{"destination-email"});
    CHECK(fixture.methodTransport.sourceSetCalls == 1);
    CHECK(fixture.sourceStillExists());
    CHECK(fixture.pinCount() == 0);
}

TEST_CASE("ambiguous source cleanup blocks move without retrying or compensating destination",
          "[app][mail-transfer][executor][move][source-ambiguity]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    fixture.methodTransport.sourceCleanupBehavior = SourceCleanupBehavior::DispatchedFailure;
    const auto operationId = fixture.prepare(MailTransferOperation::Move);

    const auto first = fixture.execute(operationId);
    REQUIRE(std::holds_alternative<MailTransferExecutionSummary>(first));
    CHECK(std::get<MailTransferExecutionSummary>(first).status ==
          MailTransferStatus::BlockedUnknown);
    const auto item = fixture.item(operationId);
    CHECK(item.phase == MailTransferItemPhase::SourceCleanupUnknown);
    CHECK(item.destinationEmailId == std::optional<std::string>{"destination-email"});
    CHECK(fixture.pinCount() == 1);
    const int sourceSetCount = fixture.methodTransport.sourceSetCalls;
    const int importCount = fixture.methodTransport.importCalls;

    fixture.methodTransport.sourceCleanupBehavior = SourceCleanupBehavior::Success;
    const auto second = fixture.execute(operationId);
    REQUIRE(std::holds_alternative<MailTransferExecutionSummary>(second));
    CHECK(std::get<MailTransferExecutionSummary>(second).status ==
          MailTransferStatus::BlockedUnknown);
    CHECK(fixture.methodTransport.sourceSetCalls == sourceSetCount);
    CHECK(fixture.methodTransport.importCalls == importCount);
    CHECK(fixture.pinCount() == 1);
}

TEST_CASE("source already absent completes move without claiming or repeating deletion",
          "[app][mail-transfer][executor][move][source-reconcile]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    fixture.methodTransport.sourceCleanupBehavior = SourceCleanupBehavior::NotFound;
    const auto operationId = fixture.prepare(MailTransferOperation::Move);

    const auto result = fixture.execute(operationId);
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        FAIL(error->message.toStdString());
    CHECK(std::get<MailTransferExecutionSummary>(result).status == MailTransferStatus::Complete);
    const auto item = fixture.item(operationId);
    CHECK(item.phase == MailTransferItemPhase::Complete);
    CHECK_FALSE(item.sourceDestroy);
    CHECK(item.sourceRemoveMailboxIds.empty());
    CHECK(fixture.methodTransport.sourceGetCalls == 1);
    CHECK(fixture.methodTransport.sourceSetCalls == 0);
    CHECK(fixture.pinCount() == 0);
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
    fixture.methodTransport.reconciliationCreatedIds.clear();
    const auto second = fixture.execute(operationId);
    REQUIRE(std::holds_alternative<MailTransferExecutionSummary>(second));
    CHECK(std::get<MailTransferExecutionSummary>(second).status == MailTransferStatus::Running);
    CHECK(fixture.item(operationId).phase == MailTransferItemPhase::Uploaded);
    CHECK(fixture.methodTransport.changesCalls == 1);
    CHECK(fixture.resourceTransport.sendFromFileCalls == uploadCount);
    CHECK(fixture.methodTransport.importCalls == importCount);
    CHECK(fixture.sourceStillExists());

    const auto third = fixture.execute(operationId);
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&third))
        FAIL(error->message.toStdString());
    CHECK(std::get<MailTransferExecutionSummary>(third).status == MailTransferStatus::Complete);
    CHECK(fixture.resourceTransport.sendFromFileCalls == uploadCount);
    CHECK(fixture.methodTransport.importCalls == importCount + 1);
    CHECK_FALSE(fixture.sourceStillExists());
}

TEST_CASE("unique created candidate reconciles lost Email import response without retry",
          "[app][mail-transfer][executor][ambiguity][reconcile]")
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
    const int uploadCount = fixture.resourceTransport.sendFromFileCalls;
    const int importCount = fixture.methodTransport.importCalls;

    fixture.methodTransport.reconciliationCreatedIds = {"reconciled-email"};
    fixture.methodTransport.reconciliationCandidateMatchesSource = true;
    const auto second = fixture.execute(operationId);
    REQUIRE(std::holds_alternative<MailTransferExecutionSummary>(second));
    CHECK(std::get<MailTransferExecutionSummary>(second).status == MailTransferStatus::Running);
    const auto reconciledItem = fixture.item(operationId);
    CHECK(reconciledItem.phase == MailTransferItemPhase::DestinationConfirmed);
    CHECK(reconciledItem.destinationEmailId ==
          std::optional<std::string>{"reconciled-email"});
    CHECK(fixture.methodTransport.changesCalls == 1);
    CHECK(fixture.methodTransport.candidateGetCalls == 1);
    CHECK(fixture.resourceTransport.sendFromFileCalls == uploadCount);
    CHECK(fixture.methodTransport.importCalls == importCount);
    CHECK(fixture.sourceStillExists());

    const auto third = fixture.execute(operationId);
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&third))
        FAIL(error->message.toStdString());
    CHECK(std::get<MailTransferExecutionSummary>(third).status == MailTransferStatus::Complete);
    CHECK(fixture.resourceTransport.sendFromFileCalls == uploadCount);
    CHECK(fixture.methodTransport.importCalls == importCount);
    CHECK_FALSE(fixture.sourceStillExists());
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
    CHECK(fixture.methodTransport.existingGetCalls == 2);
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
    CHECK(fixture.methodTransport.existingGetCalls == 2);
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
    const int uploadCount = fixture.resourceTransport.sendFromFileCalls;

    fixture.methodTransport.behavior = ImportBehavior::AlreadyExistsNeedsMembership;
    const auto second = fixture.execute(operationId);
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&second))
        FAIL(error->message.toStdString());
    CHECK(std::get<MailTransferExecutionSummary>(second).status == MailTransferStatus::Complete);
    CHECK(fixture.methodTransport.setCalls == setCount + 1);
    CHECK(fixture.methodTransport.importCalls == importCount + 1);
    CHECK(fixture.resourceTransport.sendFromFileCalls == uploadCount);
    CHECK(fixture.item(operationId).phase == MailTransferItemPhase::Complete);
    CHECK_FALSE(fixture.sourceStillExists());
    CHECK(fixture.pinCount() == 1);
}

TEST_CASE("advanced destination state keeps ambiguous duplicate mailbox update blocked",
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
    const int setCount = fixture.methodTransport.setCalls;
    const int importCount = fixture.methodTransport.importCalls;
    const int uploadCount = fixture.resourceTransport.sendFromFileCalls;

    fixture.methodTransport.behavior = ImportBehavior::AlreadyExistsNeedsMembership;
    fixture.methodTransport.existingState = "destination-state-later";
    const auto second = fixture.execute(operationId);
    REQUIRE(std::holds_alternative<MailTransferExecutionSummary>(second));
    CHECK(std::get<MailTransferExecutionSummary>(second).status ==
          MailTransferStatus::BlockedUnknown);
    CHECK(fixture.item(operationId).phase == MailTransferItemPhase::DestinationUnknown);
    CHECK(fixture.methodTransport.setCalls == setCount);
    CHECK(fixture.methodTransport.importCalls == importCount);
    CHECK(fixture.resourceTransport.sendFromFileCalls == uploadCount);
    CHECK(fixture.sourceStillExists());
    CHECK(fixture.pinCount() == 1);
}

TEST_CASE("completed destructive move publishes one durable transfer history entry and hands off MIME",
          "[app][mail-transfer][history]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    const auto operationId = fixture.prepare(MailTransferOperation::Move);
    javelin::app::undo::HistoryRepository historyRepository{fixture.database};
    javelin::app::undo::UndoManager undoManager{historyRepository};
    REQUIRE_FALSE(undoManager.load().has_value());
    javelin::app::undo::MailTransferHistoryCoordinator coordinator{fixture.database, undoManager};
    MailTransferExecutor executor{fixture.database, fixture.resourceTransport, fixture.methodTransport,
                                  *fixture.contentClient, fixture.connections, &coordinator};
    const auto executed = QCoro::waitFor(executor.advance(operationId));
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&executed))
        FAIL(error->message.toStdString());
    REQUIRE(std::holds_alternative<MailTransferExecutionSummary>(executed));
    const auto& executionSummary = std::get<MailTransferExecutionSummary>(executed);
    CHECK(executionSummary.status == MailTransferStatus::Complete);
    const auto historyEntryId = executionSummary.historyEntryId;
    CHECK(fixture.pinCount() == 1);
    REQUIRE(historyEntryId.has_value());
    REQUIRE(undoManager.entries().size() == 1);
    const auto& entry = undoManager.entries().front();
    CHECK(entry.entryId == *historyEntryId);
    CHECK(entry.commandKind == QStringLiteral("mail_transfer"));
    CHECK(entry.status == javelin::app::undo::HistoryEntryStatus::Ready);
    REQUIRE(std::holds_alternative<javelin::app::undo::MailTransferHistory>(entry.payload));
    const auto& history = std::get<javelin::app::undo::MailTransferHistory>(entry.payload);
    CHECK(history.operation == javelin::app::undo::MailTransferHistoryOperation::Move);
    CHECK(history.sourceAccountId == fixture.sourceAccountId);
    CHECK(history.destinationAccountId == fixture.destinationAccountId);
    CHECK(history.destinationMailboxId == "archive");
    REQUIRE(history.items.size() == 1);
    const auto& historyItem = history.items.front();
    CHECK_FALSE(historyItem.currentSourceEmailId.has_value());
    CHECK(historyItem.originalSourceMailboxIds == std::vector<std::string>{"inbox"});
    CHECK(historyItem.sourceDestroyed);
    REQUIRE(historyItem.rawContentHash.has_value());
    CHECK(historyItem.currentDestinationEmailId ==
          std::optional<std::string>{"destination-email"});
    CHECK(historyItem.destinationMailboxIds == std::vector<std::string>{"archive"});
    CHECK(historyItem.destinationKeywords == std::vector<std::string>{"$flagged", "$seen"});

    QSqlQuery pins{fixture.database.database()};
    pins.prepare(QStringLiteral(
        "SELECT owner_kind,owner_id FROM mail_vault_pins WHERE content_hash=:hash"));
    pins.bindValue(QStringLiteral(":hash"),
                   QString::fromStdString(*historyItem.rawContentHash));
    REQUIRE(pins.exec());
    REQUIRE(pins.next());
    CHECK(pins.value(0).toString() == QStringLiteral("history_entry"));
    CHECK(pins.value(1).toString() == *historyEntryId);
    CHECK_FALSE(pins.next());

    const auto storedOperation = fixture.operation(operationId);
    CHECK(storedOperation.historyEntryId == historyEntryId);

    const auto repeated = coordinator.finalizeCompleted(operationId);
    REQUIRE(std::holds_alternative<std::optional<QString>>(repeated));
    CHECK(std::get<std::optional<QString>>(repeated) == historyEntryId);
    CHECK(undoManager.entries().size() == 1);
}

TEST_CASE("forgotten transfer history releases MIME and is never resurrected",
          "[app][mail-transfer][history][retention]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    const auto operationId = fixture.prepare(MailTransferOperation::Move);
    const auto executed = fixture.execute(operationId);
    REQUIRE(std::holds_alternative<MailTransferExecutionSummary>(executed));

    javelin::app::undo::HistoryRepository historyRepository{fixture.database};
    javelin::app::undo::UndoManager undoManager{historyRepository};
    REQUIRE_FALSE(undoManager.load().has_value());
    javelin::app::undo::MailTransferHistoryCoordinator coordinator{fixture.database, undoManager};
    const auto finalized = coordinator.finalizeCompleted(operationId);
    REQUIRE(std::holds_alternative<std::optional<QString>>(finalized));
    const auto historyEntryId = std::get<std::optional<QString>>(finalized);
    REQUIRE(historyEntryId.has_value());
    CHECK(fixture.pinCount() == 1);

    REQUIRE_FALSE(undoManager.forget(*historyEntryId).has_value());
    CHECK(undoManager.entries().empty());
    CHECK(fixture.pinCount() == 0);
    CHECK(fixture.operation(operationId).historyEntryId == historyEntryId);

    const auto repeated = coordinator.finalizeCompleted(operationId);
    REQUIRE(std::holds_alternative<std::optional<QString>>(repeated));
    CHECK_FALSE(std::get<std::optional<QString>>(repeated).has_value());
    CHECK(undoManager.entries().empty());
    CHECK(fixture.pinCount() == 0);
}

TEST_CASE("history Redo can retain current source MIME before destructive existing-destination cleanup",
          "[app][mail-transfer][history][redo][retention]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    const auto historyEntryId =
        fixture.createTransferHistoryEntry(QStringLiteral("history-redo-retain"));
    javelin::app::undo::MailTransferHistoryService service{
        fixture.database, fixture.resourceTransport, fixture.methodTransport, fixture.connections};

    const auto retained = QCoro::waitFor(service.retainSourceForHistory(
        historyEntryId, fixture.sourceAccountId, "email-1"));
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&retained))
        FAIL(error->message.toStdString());
    const auto hash = std::get<std::string>(retained);
    REQUIRE_FALSE(hash.empty());

    QSqlQuery pin{fixture.database.database()};
    pin.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM mail_vault_pins WHERE owner_kind='history_entry' AND "
        "owner_id=:owner_id AND content_hash=:content_hash"));
    pin.bindValue(QStringLiteral(":owner_id"), historyEntryId);
    pin.bindValue(QStringLiteral(":content_hash"), QString::fromStdString(hash));
    REQUIRE(pin.exec());
    REQUIRE(pin.next());
    CHECK(pin.value(0).toInt() == 1);

    const auto repeated = QCoro::waitFor(service.retainSourceForHistory(
        historyEntryId, fixture.sourceAccountId, "email-1"));
    REQUIRE(std::holds_alternative<std::string>(repeated));
    CHECK(std::get<std::string>(repeated) == hash);
    REQUIRE(pin.exec());
    REQUIRE(pin.next());
    CHECK(pin.value(0).toInt() == 1);
}

TEST_CASE("history Redo reuses one durable forward transfer for the same generation",
          "[app][mail-transfer][history][redo]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    const auto historyEntryId =
        fixture.createTransferHistoryEntry(QStringLiteral("history-redo-copy"));
    javelin::app::undo::MailTransferHistoryService service{
        fixture.database, fixture.resourceTransport, fixture.methodTransport, fixture.connections};
    javelin::app::undo::MailTransferItemHistory historyItem{
        .currentSourceEmailId = std::optional<std::string>{"email-1"},
        .originalSourceMailboxIds = {"inbox"},
        .sourceKeywords = {"$seen", "$flagged"},
        .sourceMessageIds = {"mail-transfer@example.test"},
        .sourceReceivedAt = std::optional<std::string>{"2026-08-15T08:00:00Z"},
        .sourceSize = 2048,
        .sourceRemovedMailboxIds = {"inbox"},
        .sourceDestroyed = false,
        .rawContentHash = std::nullopt,
        .currentDestinationEmailId = std::nullopt,
        .destinationReusedExisting = false,
        .destinationPriorMailboxIds = {},
        .destinationMailboxIds = {},
        .destinationKeywords = {},
        .redoGeneration = 1,
    };

    const auto first = QCoro::waitFor(service.redoMissingDestination(
        historyEntryId, javelin::app::undo::MailTransferHistoryOperation::Copy,
        fixture.sourceAccountId, fixture.destinationAccountId, "archive", historyItem));
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&first))
        FAIL(error->message.toStdString());
    const auto& firstItem = std::get<javelin::app::undo::MailTransferItemHistory>(first);
    CHECK(firstItem.currentDestinationEmailId ==
          std::optional<std::string>{"destination-email"});
    CHECK(firstItem.currentSourceEmailId == std::optional<std::string>{"email-1"});
    CHECK_FALSE(firstItem.sourceDestroyed);
    const int uploadCount = fixture.resourceTransport.sendFromFileCalls;
    const int importCount = fixture.methodTransport.importCalls;
    CHECK(uploadCount == 1);
    CHECK(importCount == 1);

    const auto second = QCoro::waitFor(service.redoMissingDestination(
        historyEntryId, javelin::app::undo::MailTransferHistoryOperation::Copy,
        fixture.sourceAccountId, fixture.destinationAccountId, "archive", historyItem));
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&second))
        FAIL(error->message.toStdString());
    CHECK(std::get<javelin::app::undo::MailTransferItemHistory>(second)
              .currentDestinationEmailId == firstItem.currentDestinationEmailId);
    CHECK(fixture.resourceTransport.sendFromFileCalls == uploadCount);
    CHECK(fixture.methodTransport.importCalls == importCount);

    QSqlQuery redoCount{fixture.database.database()};
    redoCount.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM mail_transfer_history_redos WHERE history_entry_id=:entry_id AND "
        "redo_generation=1"));
    redoCount.bindValue(QStringLiteral(":entry_id"), historyEntryId);
    REQUIRE(redoCount.exec());
    REQUIRE(redoCount.next());
    CHECK(redoCount.value(0).toInt() == 1);
}

TEST_CASE("history Redo forward transfer preserves later source mailbox via exact cleanup",
          "[app][mail-transfer][history][redo][move]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    fixture.setSourceMailboxIds({"inbox", "new-mailbox"});
    const auto historyEntryId =
        fixture.createTransferHistoryEntry(QStringLiteral("history-redo-move"));
    javelin::app::undo::MailTransferHistoryService service{
        fixture.database, fixture.resourceTransport, fixture.methodTransport, fixture.connections};
    javelin::app::undo::MailTransferItemHistory historyItem{
        .currentSourceEmailId = std::optional<std::string>{"email-1"},
        .originalSourceMailboxIds = {"inbox", "new-mailbox"},
        .sourceKeywords = {"$seen", "$flagged"},
        .sourceMessageIds = {"mail-transfer@example.test"},
        .sourceReceivedAt = std::optional<std::string>{"2026-08-15T08:00:00Z"},
        .sourceSize = 2048,
        .sourceRemovedMailboxIds = {"inbox"},
        .sourceDestroyed = false,
        .rawContentHash = std::nullopt,
        .currentDestinationEmailId = std::nullopt,
        .destinationReusedExisting = false,
        .destinationPriorMailboxIds = {},
        .destinationMailboxIds = {},
        .destinationKeywords = {},
        .redoGeneration = 1,
    };

    const auto result = QCoro::waitFor(service.redoMissingDestination(
        historyEntryId, javelin::app::undo::MailTransferHistoryOperation::Move,
        fixture.sourceAccountId, fixture.destinationAccountId, "archive", historyItem));
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        FAIL(error->message.toStdString());
    const auto& redone = std::get<javelin::app::undo::MailTransferItemHistory>(result);
    CHECK_FALSE(redone.sourceDestroyed);
    CHECK(redone.currentSourceEmailId == std::optional<std::string>{"email-1"});
    CHECK(redone.sourceRemovedMailboxIds == std::vector<std::string>{"inbox"});
    CHECK(redone.originalSourceMailboxIds ==
          std::vector<std::string>{"inbox", "new-mailbox"});
    REQUIRE(fixture.sourceMailboxIds().has_value());
    CHECK(*fixture.sourceMailboxIds() == std::vector<std::string>{"new-mailbox"});
    CHECK(fixture.methodTransport.sourceSetArguments.find("mailboxIds/inbox") !=
          std::string::npos);
    CHECK(fixture.methodTransport.sourceSetArguments.find("\"destroy\"") == std::string::npos);
    const int importCount = fixture.methodTransport.importCalls;
    const int sourceSetCount = fixture.methodTransport.sourceSetCalls;
    const int uploadCount = fixture.resourceTransport.sendFromFileCalls;
    const auto repeated = QCoro::waitFor(service.redoMissingDestination(
        historyEntryId, javelin::app::undo::MailTransferHistoryOperation::Move,
        fixture.sourceAccountId, fixture.destinationAccountId, "archive", historyItem));
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&repeated))
        FAIL(error->message.toStdString());
    CHECK(fixture.methodTransport.importCalls == importCount);
    CHECK(fixture.methodTransport.sourceSetCalls == sourceSetCount);
    CHECK(fixture.resourceTransport.sendFromFileCalls == uploadCount);
}

TEST_CASE("history Redo destructive forward move hands current MIME back to existing history",
          "[app][mail-transfer][history][redo][move][destroy]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    const auto historyEntryId =
        fixture.createTransferHistoryEntry(QStringLiteral("history-redo-destroy"));
    javelin::app::undo::MailTransferHistoryService service{
        fixture.database, fixture.resourceTransport, fixture.methodTransport, fixture.connections};
    javelin::app::undo::MailTransferItemHistory historyItem{
        .currentSourceEmailId = std::optional<std::string>{"email-1"},
        .originalSourceMailboxIds = {"inbox"},
        .sourceKeywords = {"$seen", "$flagged"},
        .sourceMessageIds = {"mail-transfer@example.test"},
        .sourceReceivedAt = std::optional<std::string>{"2026-08-15T08:00:00Z"},
        .sourceSize = 2048,
        .sourceRemovedMailboxIds = {"inbox"},
        .sourceDestroyed = false,
        .rawContentHash = std::nullopt,
        .currentDestinationEmailId = std::nullopt,
        .destinationReusedExisting = false,
        .destinationPriorMailboxIds = {},
        .destinationMailboxIds = {},
        .destinationKeywords = {},
        .redoGeneration = 1,
    };

    const auto result = QCoro::waitFor(service.redoMissingDestination(
        historyEntryId, javelin::app::undo::MailTransferHistoryOperation::Move,
        fixture.sourceAccountId, fixture.destinationAccountId, "archive", historyItem));
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        FAIL(error->message.toStdString());
    const auto& redone = std::get<javelin::app::undo::MailTransferItemHistory>(result);
    CHECK(redone.sourceDestroyed);
    CHECK_FALSE(redone.currentSourceEmailId.has_value());
    REQUIRE(redone.rawContentHash.has_value());
    CHECK_FALSE(fixture.sourceStillExists());

    QSqlQuery pin{fixture.database.database()};
    pin.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM mail_vault_pins WHERE owner_kind='history_entry' AND "
        "owner_id=:owner_id AND content_hash=:content_hash"));
    pin.bindValue(QStringLiteral(":owner_id"), historyEntryId);
    pin.bindValue(QStringLiteral(":content_hash"),
                  QString::fromStdString(*redone.rawContentHash));
    REQUIRE(pin.exec());
    REQUIRE(pin.next());
    CHECK(pin.value(0).toInt() == 1);

    const int importCount = fixture.methodTransport.importCalls;
    const int sourceSetCount = fixture.methodTransport.sourceSetCalls;
    const int uploadCount = fixture.resourceTransport.sendFromFileCalls;
    const auto repeated = QCoro::waitFor(service.redoMissingDestination(
        historyEntryId, javelin::app::undo::MailTransferHistoryOperation::Move,
        fixture.sourceAccountId, fixture.destinationAccountId, "archive", historyItem));
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&repeated))
        FAIL(error->message.toStdString());
    CHECK(std::get<javelin::app::undo::MailTransferItemHistory>(repeated).sourceDestroyed);
    CHECK(fixture.methodTransport.importCalls == importCount);
    CHECK(fixture.methodTransport.sourceSetCalls == sourceSetCount);
    CHECK(fixture.resourceTransport.sendFromFileCalls == uploadCount);
}

TEST_CASE("same-session copy uses Email copy without MIME download or upload",
          "[app][mail-transfer][executor][same-session]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    SameSessionFixture fixture;
    const auto operationId = fixture.prepare(MailTransferOperation::Copy);

    const auto result = fixture.execute(operationId);
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        FAIL(error->message.toStdString());
    CHECK(std::get<MailTransferExecutionSummary>(result).status == MailTransferStatus::Complete);
    CHECK(fixture.methodTransport.copyCalls == 1);
    CHECK(fixture.methodTransport.sourceGetCalls == 0);
    CHECK(fixture.methodTransport.sourceSetCalls == 0);
    CHECK(fixture.resourceTransport.sendCalls == 0);
    CHECK(fixture.resourceTransport.sendFromFileCalls == 0);

    const auto arguments = QJsonDocument::fromJson(
                               QByteArray::fromStdString(fixture.methodTransport.copyArguments))
                               .object();
    CHECK(arguments.value(QStringLiteral("fromAccountId")).toString() == QStringLiteral("u1"));
    CHECK(arguments.value(QStringLiteral("accountId")).toString() == QStringLiteral("u2"));
    CHECK_FALSE(arguments.value(QStringLiteral("onSuccessDestroyOriginal")).toBool(true));
    const auto create = arguments.value(QStringLiteral("create")).toObject();
    REQUIRE(create.size() == 1);
    const auto copy = create.begin().value().toObject();
    CHECK(copy.value(QStringLiteral("id")).toString() == QStringLiteral("email-1"));
    CHECK(copy.value(QStringLiteral("mailboxIds"))
              .toObject()
              .value(QStringLiteral("archive"))
              .toBool());
    CHECK(copy.value(QStringLiteral("keywords"))
              .toObject()
              .value(QStringLiteral("$seen"))
              .toBool());

    const auto item = fixture.item(operationId);
    CHECK(item.phase == MailTransferItemPhase::Complete);
    CHECK_FALSE(item.rawContentHash.has_value());
    CHECK(fixture.pinCount() == 0);
    REQUIRE(fixture.sourceMailboxIds().has_value());
    CHECK(*fixture.sourceMailboxIds() == std::vector<std::string>{"inbox"});
}

TEST_CASE("same-session mailbox-only move stays MIME-free and removes exact source membership",
          "[app][mail-transfer][executor][same-session][move]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    SameSessionFixture fixture;
    fixture.setSourceMailboxIds({"inbox", "important"});
    const auto operationId = fixture.prepare(MailTransferOperation::Move);

    const auto result = fixture.execute(operationId);
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        FAIL(error->message.toStdString());
    CHECK(std::get<MailTransferExecutionSummary>(result).status == MailTransferStatus::Complete);
    CHECK(fixture.methodTransport.copyCalls == 1);
    CHECK(fixture.methodTransport.sourceGetCalls == 1);
    CHECK(fixture.methodTransport.sourceSetCalls == 1);
    CHECK(fixture.resourceTransport.sendCalls == 0);
    CHECK(fixture.resourceTransport.sendFromFileCalls == 0);
    CHECK(fixture.methodTransport.sourceSetArguments.find("mailboxIds/inbox") !=
          std::string::npos);
    CHECK(fixture.methodTransport.sourceSetArguments.find("\"destroy\"") == std::string::npos);
    const auto item = fixture.item(operationId);
    CHECK(item.phase == MailTransferItemPhase::Complete);
    CHECK_FALSE(item.sourceDestroy);
    CHECK_FALSE(item.rawContentHash.has_value());
    CHECK(fixture.pinCount() == 0);
    REQUIRE(fixture.sourceMailboxIds().has_value());
    CHECK(*fixture.sourceMailboxIds() == std::vector<std::string>{"important"});
}

TEST_CASE("same-session destructive move retains raw MIME only immediately before source destroy",
          "[app][mail-transfer][executor][same-session][move][undo-retention]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    SameSessionFixture fixture;
    fixture.seedRawSource();
    const auto operationId = fixture.prepare(MailTransferOperation::Move);
    CHECK_FALSE(fixture.item(operationId).rawContentHash.has_value());

    const auto result = fixture.execute(operationId);
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        FAIL(error->message.toStdString());
    CHECK(std::get<MailTransferExecutionSummary>(result).status == MailTransferStatus::Complete);
    CHECK(fixture.methodTransport.copyCalls == 1);
    CHECK(fixture.methodTransport.sourceSetCalls == 1);
    CHECK(fixture.resourceTransport.sendCalls == 0);
    CHECK(fixture.resourceTransport.sendFromFileCalls == 0);
    CHECK(fixture.methodTransport.sourceSetArguments.find("\"destroy\":[\"email-1\"]") !=
          std::string::npos);
    const auto item = fixture.item(operationId);
    CHECK(item.phase == MailTransferItemPhase::Complete);
    CHECK(item.sourceDestroy);
    CHECK(item.rawContentHash.has_value());
    CHECK(fixture.pinCount() == 1);
    CHECK_FALSE(fixture.sourceMailboxIds().has_value());
}

TEST_CASE("lost Email copy response blocks same-session transfer without source cleanup or retry",
          "[app][mail-transfer][executor][same-session][ambiguity]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    SameSessionFixture fixture;
    fixture.methodTransport.copyDispatchedFailure = true;
    const auto operationId = fixture.prepare(MailTransferOperation::Move);

    const auto first = fixture.execute(operationId);
    REQUIRE(std::holds_alternative<MailTransferExecutionSummary>(first));
    CHECK(std::get<MailTransferExecutionSummary>(first).status ==
          MailTransferStatus::BlockedUnknown);
    CHECK(fixture.item(operationId).phase == MailTransferItemPhase::DestinationUnknown);
    CHECK(fixture.methodTransport.copyCalls == 1);
    CHECK(fixture.methodTransport.sourceGetCalls == 0);
    CHECK(fixture.methodTransport.sourceSetCalls == 0);
    CHECK(fixture.resourceTransport.sendCalls == 0);
    CHECK(fixture.resourceTransport.sendFromFileCalls == 0);

    fixture.methodTransport.copyDispatchedFailure = false;
    fixture.methodTransport.reconciliationCreatedIds = {"unrelated-shared"};
    const auto second = fixture.execute(operationId);
    REQUIRE(std::holds_alternative<MailTransferExecutionSummary>(second));
    CHECK(std::get<MailTransferExecutionSummary>(second).status ==
          MailTransferStatus::BlockedUnknown);
    CHECK(fixture.methodTransport.changesCalls == 1);
    CHECK(fixture.methodTransport.candidateGetCalls == 1);
    CHECK(fixture.methodTransport.copyCalls == 1);
    CHECK(fixture.methodTransport.sourceSetCalls == 0);
}
