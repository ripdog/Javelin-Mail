#include "app/undo/MailTransferHistoryService.h"
#include "app/AccountConnectionProvider.h"
#include "app/undo/HistoryRepository.h"
#include "app/undo/HistoryTypes.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/Session.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/RawMessageSourceRepository.h"
#include "jmap/cache/SessionRepository.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
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
    using namespace javelin::app::undo;
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
            static char name[] = "javelin-tests";
            static char* argv[] = {name, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    [[nodiscard]] javelin::jmap::cache::DatabaseConnection openDatabase(const QString& path)
    {
        auto opened = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = QStringLiteral("mail-transfer-history-service-%1")
                                  .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)),
            .databasePath = path,
        });
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
            FAIL(error->message.toStdString());
        return std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    }

    [[nodiscard]] javelin::jmap::api::Session session()
    {
        javelin::jmap::api::Session value{
            .username = "source@example.test",
            .apiUrl = "https://source.example.test/jmap",
            .downloadUrl = "https://source.example.test/download/{accountId}/{blobId}/{name}",
            .uploadUrl = "https://source.example.test/upload/{accountId}",
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
            .primaryAccounts = {.mailAccountId = "u1",
                                .submissionAccountId = std::nullopt,
                                .contactsAccountId = std::nullopt,
                                .calendarsAccountId = std::nullopt,
                                .sieveAccountId = std::nullopt},
        };
        value.accounts.emplace("u1", javelin::jmap::api::Account{
                                         .id = "u1",
                                         .name = "Source",
                                         .isPersonal = true,
                                         .isReadOnly = false,
                                         .accountCapabilities =
                                             {
                                                 .mail = true,
                                                 .mailDetails =
                                                     javelin::jmap::api::MailAccountCapability{
                                                         .mayCreateTopLevelMailbox = true},
                                                 .submission = std::nullopt,
                                                 .contacts = std::nullopt,
                                                 .calendars = std::nullopt,
                                                 .sieve = false,
                                             },
                                     });
        return value;
    }

    class ConnectionProvider final : public AccountConnectionProvider
    {
      public:
        std::unordered_map<std::string, AccountConnectionSettings> settings;

        [[nodiscard]] std::optional<AccountConnectionSettings>
        connectionSettingsFor(std::string_view accountId) const override
        {
            const auto found = settings.find(std::string{accountId});
            return found == settings.end() ? std::nullopt : std::optional{found->second};
        }
    };

    class ResourceTransport final : public javelin::jmap::api::AbstractTransport
    {
      public:
        int uploads = 0;
        QByteArray payload;

        QCoro::Task<javelin::jmap::api::TransportResult> send(HttpRequest) override
        {
            co_return TransportError{
                .code = TransportErrorCode::NetworkFailure,
                .message = "unexpected buffered request",
                .httpStatus = std::nullopt,
                .networkError = std::nullopt,
                .retryAfter = std::nullopt,
            };
        }

        QCoro::Task<javelin::jmap::api::TransportResult> sendFromFile(HttpRequest request,
                                                                      QString filePath) override
        {
            ++uploads;
            if (request.dispatched)
                request.dispatched();
            QFile file{filePath};
            REQUIRE(file.open(QIODevice::ReadOnly));
            payload = file.readAll();
            co_return HttpResponse{
                .statusCode = 200,
                .body = QByteArrayLiteral(
                    R"({"accountId":"u1","blobId":"history-uploaded-blob","type":"message/rfc822","size":58})"),
            };
        }
    };

    enum class ImportBehavior
    {
        Success,
        DispatchedFailure,
    };

    class MethodTransport final : public javelin::jmap::api::JmapMethodTransport
    {
      public:
        ImportBehavior importBehavior = ImportBehavior::Success;
        int stateCalls = 0;
        int importCalls = 0;
        int changesCalls = 0;
        int candidateCalls = 0;
        int materializeCalls = 0;
        std::vector<std::string> createdDuringReconciliation;
        bool candidateMatches = true;

        QCoro::Task<JmapMethodTransportResult> call(JmapMethodRequest request) override
        {
            REQUIRE(request.apiUrl == "https://source.example.test/jmap");
            REQUIRE(request.envelope.methodCalls.size() == 1);
            const auto& method = request.envelope.methodCalls.front();
            if (method.name == "Email/get")
            {
                if (request.dispatched)
                    request.dispatched();
                const auto arguments =
                    QJsonDocument::fromJson(QByteArray::fromStdString(method.arguments)).object();
                const auto ids = arguments.value(QStringLiteral("ids")).toArray();
                if (ids.isEmpty())
                {
                    ++stateCalls;
                    co_return ResponseEnvelope{
                        .methodResponses = {{
                            .name = "Email/get",
                            .arguments =
                                R"({"accountId":"u1","state":"source-pre-state","list":[],"notFound":[]})",
                            .callId = method.callId,
                        }},
                        .createdIds = std::nullopt,
                        .sessionState = "session-state",
                    };
                }

                const QString requestedId = ids.first().toString();
                if (requestedId == QStringLiteral("candidate-email") && candidateCalls == 0)
                    ++candidateCalls;
                else
                    ++materializeCalls;
                const bool matching =
                    requestedId != QStringLiteral("candidate-email") || candidateMatches;
                const QString messageId = matching ? QStringLiteral("restore@example.test")
                                                   : QStringLiteral("other@example.test");
                const auto response =
                    QStringLiteral(
                        R"({"accountId":"u1","state":"source-after-state","list":[{"id":"%1","blobId":"restored-blob","threadId":"restored-thread","mailboxIds":{"inbox":true,"important":true},"keywords":{"$seen":true,"project":true},"size":58,"receivedAt":"2026-08-15T00:00:00Z","messageId":["%2"],"inReplyTo":[],"references":[],"hasAttachment":false,"subject":"Restore","from":[],"to":[],"cc":[],"bcc":[],"replyTo":[],"preview":"Restored"}],"notFound":[]})")
                        .arg(requestedId, messageId)
                        .toStdString();
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
                std::string created{"["};
                for (std::size_t index = 0; index < createdDuringReconciliation.size(); ++index)
                {
                    if (index > 0)
                        created += ',';
                    created += '"' + createdDuringReconciliation[index] + '"';
                }
                created += ']';
                const auto response =
                    std::string{
                        R"({"accountId":"u1","oldState":"source-pre-state","newState":"source-after-state","hasMoreChanges":false,"created":)"} +
                    created + R"(,"updated":[],"destroyed":[]})";
                if (request.dispatched)
                    request.dispatched();
                co_return ResponseEnvelope{
                    .methodResponses = {{.name = "Email/changes",
                                         .arguments = response,
                                         .callId = method.callId}},
                    .createdIds = std::nullopt,
                    .sessionState = "session-state",
                };
            }

            REQUIRE(method.name == "Email/import");
            ++importCalls;
            if (request.dispatched)
                request.dispatched();
            if (importBehavior == ImportBehavior::DispatchedFailure)
            {
                co_return TransportError{
                    .code = TransportErrorCode::NetworkFailure,
                    .message = "connection lost after import dispatch",
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
            const auto response =
                QStringLiteral(
                    R"({"accountId":"u1","oldState":"source-pre-state","newState":"source-after-state","created":{"%1":{"id":"restored-email","blobId":"restored-blob","threadId":"restored-thread","size":58}},"notCreated":{}})")
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
        std::string accountId;
        ConnectionProvider connections;
        ResourceTransport resource;
        MethodTransport methods;
        QByteArray raw = QByteArrayLiteral(
            "Message-ID: <restore@example.test>\r\nSubject: Restore\r\n\r\nBody\r\n");
        QString historyEntryId = QStringLiteral("history-transfer-restore");
        std::string contentHash;

        Fixture()
            : database(openDatabase(
                  [this]
                  {
                      REQUIRE(directory.isValid());
                      return directory.filePath(QStringLiteral("cache.sqlite3"));
                  }()))
        {
            javelin::jmap::cache::SessionRepository sessions{database};
            const auto stored = sessions.replaceForConnection("connection-a", "u1", session());
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&stored))
                FAIL(error->message.toStdString());
            accountId = std::get<javelin::jmap::cache::StoredSessionAccounts>(stored)
                            .accountIdsByRemoteId.at("u1");
            connections.settings.emplace(
                accountId,
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

            HistoryRepository history{database};
            HistoryEntry entry;
            entry.entryId = historyEntryId;
            entry.stack = HistoryStack::Undo;
            entry.label = QStringLiteral("Move message");
            entry.domain = HistoryDomain::Mail;
            entry.commandKind = QStringLiteral("mail_transfer");
            entry.payloadVersion = 1;
            entry.payload = MailTransferHistory{
                .sourceAccountId = accountId,
                .destinationAccountId = "other-account",
                .destinationMailboxId = "archive",
                .operation = MailTransferHistoryOperation::Move,
                .items = {},
            };
            entry.status = HistoryEntryStatus::Ready;
            REQUIRE(std::holds_alternative<HistoryEntry>(history.pushUndoClearingRedo(entry)));

            javelin::jmap::cache::RawMessageSourceRepository sources{database};
            REQUIRE_FALSE(
                sources
                    .upsert(accountId,
                            {.emailId = "old-source", .blobId = "old-blob", .payload = raw})
                    .has_value());
            const auto reference = sources.findReference(accountId, "old-source");
            REQUIRE(std::holds_alternative<
                    std::optional<javelin::jmap::cache::RawMessageSourceReference>>(reference));
            const auto& found =
                std::get<std::optional<javelin::jmap::cache::RawMessageSourceReference>>(reference);
            REQUIRE(found.has_value());
            contentHash = found->object.contentHash;

            QSqlQuery pin{database.database()};
            pin.prepare(QStringLiteral(
                "INSERT INTO mail_vault_pins(owner_kind,owner_id,content_hash) VALUES("
                "'history_entry',:owner_id,:content_hash)"));
            pin.bindValue(QStringLiteral(":owner_id"), historyEntryId);
            pin.bindValue(QStringLiteral(":content_hash"), QString::fromStdString(contentHash));
            REQUIRE(pin.exec());
        }

        [[nodiscard]] RecreatedMailTransferSourceResult recreate()
        {
            MailTransferHistoryService service{database, resource, methods, connections};
            return QCoro::waitFor(service.recreateSourceFromHistory(
                historyEntryId, accountId, contentHash, {"inbox", "important"},
                {"$seen", "project"}, {"restore@example.test"},
                std::optional<std::string>{"2026-08-15T00:00:00Z"}, 58));
        }

        [[nodiscard]] QString phase() const
        {
            QSqlQuery query{database.database()};
            query.prepare(QStringLiteral(
                "SELECT phase FROM mail_transfer_history_recreations WHERE "
                "history_entry_id=:history_entry_id AND content_hash=:content_hash"));
            query.bindValue(QStringLiteral(":history_entry_id"), historyEntryId);
            query.bindValue(QStringLiteral(":content_hash"), QString::fromStdString(contentHash));
            REQUIRE(query.exec());
            REQUIRE(query.next());
            return query.value(0).toString();
        }

        [[nodiscard]] bool cached(std::string_view emailId)
        {
            javelin::jmap::cache::EmailRepository emails{database};
            const auto found = emails.find(accountId, emailId);
            REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(found));
            return std::get<std::optional<javelin::jmap::domain::Email>>(found).has_value();
        }
    };
} // namespace

TEST_CASE("history source recreation streams retained MIME and materializes restored Email",
          "[app][undo][mail-transfer][history-service]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;

    const auto result = fixture.recreate();
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        FAIL(error->message.toStdString());
    const auto& recreated = std::get<RecreatedMailTransferSource>(result);
    CHECK(recreated.emailId == "restored-email");
    CHECK(recreated.blobId == std::optional<std::string>{"restored-blob"});
    CHECK(recreated.threadId == std::optional<std::string>{"restored-thread"});
    CHECK(fixture.resource.uploads == 1);
    CHECK(fixture.resource.payload == fixture.raw);
    CHECK(fixture.methods.stateCalls == 1);
    CHECK(fixture.methods.importCalls == 1);
    CHECK(fixture.methods.materializeCalls == 1);
    CHECK(fixture.phase() == QStringLiteral("complete"));
    CHECK(fixture.cached("restored-email"));
}

TEST_CASE("unknown history import proven absent retries without uploading raw MIME again",
          "[app][undo][mail-transfer][history-service][ambiguity]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    fixture.methods.importBehavior = ImportBehavior::DispatchedFailure;

    const auto first = fixture.recreate();
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(first));
    const auto& firstError = std::get<javelin::jmap::OperationError>(first);
    CHECK(firstError.protocolType == std::optional<std::string>{"ambiguousOutcome"});
    CHECK(fixture.phase() == QStringLiteral("unknown"));
    CHECK(fixture.resource.uploads == 1);
    CHECK(fixture.methods.importCalls == 1);

    fixture.methods.importBehavior = ImportBehavior::Success;
    fixture.methods.createdDuringReconciliation.clear();
    const auto second = fixture.recreate();
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&second))
        FAIL(error->message.toStdString());
    CHECK(std::get<RecreatedMailTransferSource>(second).emailId == "restored-email");
    CHECK(fixture.methods.changesCalls == 1);
    CHECK(fixture.resource.uploads == 1);
    CHECK(fixture.methods.importCalls == 2);
    CHECK(fixture.phase() == QStringLiteral("complete"));
    CHECK(fixture.cached("restored-email"));
}

TEST_CASE("unknown history import correlates one created candidate without a second import",
          "[app][undo][mail-transfer][history-service][ambiguity][candidate]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    fixture.methods.importBehavior = ImportBehavior::DispatchedFailure;

    const auto first = fixture.recreate();
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(first));
    CHECK(fixture.phase() == QStringLiteral("unknown"));

    fixture.methods.importBehavior = ImportBehavior::Success;
    fixture.methods.createdDuringReconciliation = {"candidate-email"};
    const auto second = fixture.recreate();
    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&second))
        FAIL(error->message.toStdString());
    CHECK(std::get<RecreatedMailTransferSource>(second).emailId == "candidate-email");
    CHECK(fixture.methods.changesCalls == 1);
    CHECK(fixture.methods.candidateCalls == 1);
    CHECK(fixture.methods.importCalls == 1);
    CHECK(fixture.resource.uploads == 1);
    CHECK(fixture.phase() == QStringLiteral("complete"));
    CHECK(fixture.cached("candidate-email"));
}
