#include "app/MailImportService.h"
#include "app/AccountConnectionProvider.h"
#include "app/MailImportRepository.h"
#include "app/WorkScheduler.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/Session.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/SessionRepository.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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
            static char name[] = "mail-import-service-test";
            static char* argv[]{name, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    [[nodiscard]] bool spinUntil(const std::function<bool()>& predicate)
    {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < 3000)
        {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        QCoreApplication::processEvents();
        return predicate();
    }

    [[nodiscard]] javelin::jmap::cache::DatabaseConnection openDatabase(const QString& path)
    {
        auto opened = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = QStringLiteral("mail-import-service-%1")
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
            .username = "alice@example.test",
            .apiUrl = "https://destination.example.test/jmap",
            .downloadUrl = "https://destination.example.test/download/{accountId}/{blobId}/{name}",
            .uploadUrl = "https://destination.example.test/upload/{accountId}",
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
                                         .name = "Mail",
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

    [[nodiscard]] std::string storeSession(javelin::jmap::cache::DatabaseConnection& database)
    {
        javelin::jmap::cache::SessionRepository sessions{database};
        const auto stored = sessions.replaceForConnection("connection-a", "u1", session());
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&stored))
            FAIL(error->message.toStdString());
        return std::get<javelin::jmap::cache::StoredSessionAccounts>(stored)
            .accountIdsByRemoteId.at("u1");
    }

    [[nodiscard]] javelin::jmap::domain::Mailbox mailbox()
    {
        return {
            .id = "archive",
            .name = "Archive",
            .parentId = std::nullopt,
            .role = std::nullopt,
            .sortOrder = 0,
            .totalEmails = 0,
            .unreadEmails = 0,
            .totalThreads = 0,
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
        int uploadCalls = 0;

        [[nodiscard]] QCoro::Task<javelin::jmap::api::TransportResult> send(HttpRequest) override
        {
            co_return TransportError{.code = TransportErrorCode::NetworkFailure,
                                     .message = "unexpected buffered HTTP request"};
        }

        [[nodiscard]] QCoro::Task<javelin::jmap::api::TransportResult>
        sendFromFile(HttpRequest request, QString filePath) override
        {
            ++uploadCalls;
            if (request.dispatched)
                request.dispatched();
            QFile file{filePath};
            REQUIRE(file.open(QIODevice::ReadOnly));
            uploadedPayload = file.readAll();
            const auto body =
                QStringLiteral(
                    R"({"accountId":"u1","blobId":"uploaded-blob","type":"message/rfc822","size":%1})")
                    .arg(uploadedPayload.size())
                    .toUtf8();
            co_return HttpResponse{.statusCode = 201, .body = body};
        }
    };

    enum class ImportBehavior
    {
        Success,
        AlreadyExistsNeedsMembership,
        DispatchedFailure,
    };

    class RecordingMethodTransport final : public javelin::jmap::api::JmapMethodTransport
    {
      public:
        ImportBehavior behavior = ImportBehavior::Success;
        int stateCalls = 0;
        int importCalls = 0;
        int existingGetCalls = 0;
        int transientStateFailuresRemaining = 0;
        int setCalls = 0;
        int changesCalls = 0;
        int candidateGetCalls = 0;
        std::string importArguments;
        std::string setArguments;
        bool membershipApplied = false;
        std::vector<std::string> reconciliationCreatedIds;

        [[nodiscard]] QCoro::Task<JmapMethodTransportResult>
        call(JmapMethodRequest request) override
        {
            REQUIRE(request.apiUrl == "https://destination.example.test/jmap");
            REQUIRE_FALSE(request.envelope.methodCalls.empty());
            const auto& method = request.envelope.methodCalls.front();

            if (method.name == "Email/get")
            {
                if (request.dispatched)
                    request.dispatched();
                if (method.arguments.find("existing-email") != std::string::npos)
                {
                    ++existingGetCalls;
                    const auto mailboxIds = membershipApplied
                                                ? R"({"old-mailbox":true,"archive":true})"
                                                : R"({"old-mailbox":true})";
                    const auto response =
                        std::string{
                            R"({"accountId":"u1","state":"existing-state","list":[{"id":"existing-email","blobId":"existing-blob","threadId":"existing-thread","mailboxIds":)"} +
                        mailboxIds +
                        R"(,"keywords":{"destination-tag":true},"size":91,"receivedAt":"2026-08-14T00:00:00Z","hasAttachment":false,"subject":"Existing","from":[],"to":[],"cc":[],"bcc":[],"replyTo":[],"preview":"Existing"}],"notFound":[]})";
                    co_return ResponseEnvelope{
                        .methodResponses = {{.name = "Email/get",
                                             .arguments = response,
                                             .callId = method.callId}},
                        .createdIds = std::nullopt,
                        .sessionState = "session-state",
                    };
                }

                bool candidateLookup = false;
                for (const auto& id : reconciliationCreatedIds)
                    candidateLookup =
                        candidateLookup || method.arguments.find(id) != std::string::npos;
                if (candidateLookup)
                {
                    ++candidateGetCalls;
                    std::string list;
                    for (const auto& id : reconciliationCreatedIds)
                    {
                        if (method.arguments.find(id) == std::string::npos)
                            continue;
                        if (!list.empty())
                            list += ',';
                        list +=
                            std::string{R"({"id":")"} + id +
                            R"(","blobId":"uploaded-blob","threadId":"candidate-thread","mailboxIds":{"archive":true},"keywords":{},"size":57,"receivedAt":"2026-08-22T00:00:00Z","hasAttachment":false,"subject":"Imported","from":[],"to":[],"cc":[],"bcc":[],"replyTo":[],"preview":"Imported"})";
                    }
                    co_return ResponseEnvelope{
                        .methodResponses = {{
                            .name = "Email/get",
                            .arguments =
                                std::string{
                                    R"({"accountId":"u1","state":"reconciled-state","list":[)"} +
                                list + R"(],"notFound":[]})",
                            .callId = method.callId,
                        }},
                        .createdIds = std::nullopt,
                        .sessionState = "session-state",
                    };
                }

                ++stateCalls;
                if (transientStateFailuresRemaining > 0)
                {
                    --transientStateFailuresRemaining;
                    co_return TransportError{.code = TransportErrorCode::NetworkFailure,
                                             .message = "temporary state lookup failure"};
                }
                co_return ResponseEnvelope{
                    .methodResponses = {{
                        .name = "Email/get",
                        .arguments =
                            R"({"accountId":"u1","state":"state-1","list":[],"notFound":[]})",
                        .callId = method.callId,
                    }},
                    .createdIds = std::nullopt,
                    .sessionState = "session-state",
                };
            }

            if (method.name == "Email/import")
            {
                ++importCalls;
                importArguments = method.arguments;
                if (request.dispatched)
                    request.dispatched();
                if (behavior == ImportBehavior::DispatchedFailure)
                {
                    co_return TransportError{.code = TransportErrorCode::NetworkFailure,
                                             .message = "connection lost after import dispatch"};
                }
                const auto arguments =
                    QJsonDocument::fromJson(QByteArray::fromStdString(method.arguments)).object();
                const auto emails = arguments.value(QStringLiteral("emails")).toObject();
                REQUIRE(emails.size() == 1);
                const auto creationId = emails.begin().key();
                if (behavior == ImportBehavior::AlreadyExistsNeedsMembership)
                {
                    const auto response =
                        QStringLiteral(
                            R"({"accountId":"u1","oldState":"state-1","newState":"state-1","created":{},"notCreated":{"%1":{"type":"alreadyExists","existingId":"existing-email","properties":[]}}})")
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
                const auto response =
                    QStringLiteral(
                        R"({"accountId":"u1","oldState":"state-1","newState":"state-2","created":{"%1":{"id":"created-email","blobId":"uploaded-blob","threadId":"created-thread","size":57}},"notCreated":{}})")
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

            if (method.name == "Email/set")
            {
                ++setCalls;
                setArguments = method.arguments;
                membershipApplied = true;
                if (request.dispatched)
                    request.dispatched();
                std::vector<javelin::jmap::api::MethodInvocation> responses{{
                    .name = "Email/set",
                    .arguments =
                        R"({"accountId":"u1","oldState":"existing-state","newState":"membership-state","created":{},"updated":{"existing-email":null},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{}})",
                    .callId = method.callId,
                }};
                for (std::size_t index = 1; index < request.envelope.methodCalls.size(); ++index)
                {
                    const auto& extra = request.envelope.methodCalls[index];
                    REQUIRE(extra.name == "Mailbox/get");
                    responses.push_back({.name = "error",
                                         .arguments = R"({"type":"serverUnavailable"})",
                                         .callId = extra.callId});
                }
                co_return ResponseEnvelope{
                    .methodResponses = std::move(responses),
                    .createdIds = std::nullopt,
                    .sessionState = "session-state",
                };
            }

            REQUIRE(method.name == "Email/changes");
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
                    .arguments =
                        QStringLiteral(
                            R"({"accountId":"u1","oldState":"%1","newState":"reconciled-state","hasMoreChanges":false,"created":[%2],"updated":[],"destroyed":[]})")
                            .arg(sinceState, QString::fromStdString(created))
                            .toStdString(),
                    .callId = method.callId,
                }},
                .createdIds = std::nullopt,
                .sessionState = "session-state",
            };
        }
    };

    struct Fixture
    {
        ApplicationGuard application;
        QTemporaryDir directory;
        javelin::jmap::cache::DatabaseConnection database;
        std::string accountId;
        ConnectionProvider connections;
        WorkScheduler scheduler;
        RecordingResourceTransport resourceTransport;
        RecordingMethodTransport methodTransport;
        std::vector<std::pair<std::string, std::string>> resyncs;
        MailImportService service;
        QString sourcePath;
        QByteArray raw =
            QByteArrayLiteral("From: sender@example.test\r\nSubject: Imported\r\n\r\nBody\r\n");

        Fixture()
            : database(openDatabase(
                  [this]
                  {
                      REQUIRE(directory.isValid());
                      return directory.filePath(QStringLiteral("cache.sqlite3"));
                  }())),
              accountId(storeSession(database)),
              scheduler(database, nullptr, std::chrono::milliseconds{0}),
              service(database, resourceTransport, methodTransport, connections, scheduler,
                      [this](const std::string_view account, const std::string_view mailboxId)
                      { resyncs.emplace_back(account, mailboxId); })
        {
            connections.settings.emplace(
                accountId,
                AccountConnectionSettings{.connectionId = "connection-a",
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
            REQUIRE_FALSE(mailboxes.replaceAll(accountId, {mailbox()}).has_value());
            sourcePath = directory.filePath(QStringLiteral("message.eml"));
            QFile source{sourcePath};
            REQUIRE(source.open(QIODevice::WriteOnly));
            REQUIRE(source.write(raw) == raw.size());
        }

        [[nodiscard]] MailImportAdmission start()
        {
            const auto result = QCoro::waitFor(service.startImport({
                .accountId = accountId,
                .mailboxId = std::optional<std::string>{"archive"},
                .sourcePaths = {sourcePath},
                .recreateHierarchy = false,
            }));
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                FAIL(error->message.toStdString());
            return std::get<MailImportAdmission>(result);
        }

        [[nodiscard]] MailImportOperationRecord operation(const std::string_view operationId)
        {
            MailImportRepository repository{database};
            const auto result = repository.findOperation(operationId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                FAIL(error->message.toStdString());
            const auto& value = std::get<std::optional<MailImportOperationRecord>>(result);
            REQUIRE(value.has_value());
            return *value;
        }

        [[nodiscard]] MailImportProgressSnapshot progress(const std::string_view operationId)
        {
            MailImportRepository repository{database};
            const auto result = repository.progress(operationId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                FAIL(error->message.toStdString());
            return std::get<MailImportProgressSnapshot>(result);
        }

        void captureState(const MailImportAdmission& admission)
        {
            const auto current = operation(admission.operationId);
            const auto snapshot = progress(admission.operationId);
            const auto jobResult = scheduler.find(admission.jobId);
            const auto metrics = scheduler.admissionMetrics();
            WARN("import state status="
                 << static_cast<int>(current.status) << " sealed=" << current.scanSealed
                 << " total=" << snapshot.totalItems << " completed=" << snapshot.completedItems
                 << " created=" << snapshot.createdItems << " reused=" << snapshot.reusedItems
                 << " failed=" << snapshot.failedItems << " unknown=" << snapshot.unknownItems
                 << " uploads=" << resourceTransport.uploadCalls << " stateCalls="
                 << methodTransport.stateCalls << " importCalls=" << methodTransport.importCalls
                 << " existingGets=" << methodTransport.existingGetCalls << " setCalls="
                 << methodTransport.setCalls << " changesCalls=" << methodTransport.changesCalls
                 << " candidateGets=" << methodTransport.candidateGetCalls << " admissions="
                 << scheduler.activeAdmissions() << " admitted=" << metrics.admitted
                 << " rejected=" << metrics.rejected << " released=" << metrics.completed);
            if (const auto* job = std::get_if<std::optional<WorkRecord>>(&jobResult);
                job != nullptr && job->has_value())
            {
                const auto& record = **job;
                WARN("job state status=" << static_cast<int>(record.status)
                                         << " paused=" << record.pauseRequested << " completed="
                                         << record.progress.completedUnits << " total="
                                         << (record.progress.totalUnits.has_value()
                                                 ? std::to_string(*record.progress.totalUnits)
                                                 : std::string{"none"}));
            }
        }
    };
} // namespace

TEST_CASE("mail import service streams raw EML and completes through Email import",
          "[app][mail-import][service]")
{
    Fixture fixture;
    const auto admission = fixture.start();
    const bool completed = spinUntil(
        [&]
        { return fixture.operation(admission.operationId).status == MailImportStatus::Complete; });
    if (!completed)
        fixture.captureState(admission);
    REQUIRE(completed);

    CHECK(fixture.resourceTransport.uploadCalls == 1);
    CHECK(fixture.resourceTransport.uploadedPayload == fixture.raw);
    CHECK(fixture.methodTransport.importCalls == 1);
    CHECK(fixture.methodTransport.importArguments.find("\"archive\":true") != std::string::npos);
    CHECK(fixture.methodTransport.importArguments.find("ifInState") == std::string::npos);
    const auto progress = fixture.progress(admission.operationId);
    CHECK(progress.createdItems == 1);
    CHECK(progress.reusedItems == 0);
    CHECK(progress.failedItems == 0);
    REQUIRE(fixture.resyncs.size() == 1);
    CHECK(fixture.resyncs.front() == std::pair{fixture.accountId, std::string{"archive"}});
}

TEST_CASE("mail import service reuses server duplicate and adds only target membership",
          "[app][mail-import][service][duplicate]")
{
    Fixture fixture;
    fixture.methodTransport.behavior = ImportBehavior::AlreadyExistsNeedsMembership;
    const auto admission = fixture.start();
    const bool completed = spinUntil(
        [&]
        { return fixture.operation(admission.operationId).status == MailImportStatus::Complete; });
    if (!completed)
        fixture.captureState(admission);
    REQUIRE(completed);

    CHECK(fixture.methodTransport.importCalls == 1);
    CHECK(fixture.methodTransport.existingGetCalls >= 1);
    CHECK(fixture.methodTransport.setCalls == 1);
    CHECK(fixture.methodTransport.setArguments.find("mailboxIds/archive") != std::string::npos);
    CHECK(fixture.methodTransport.setArguments.find("mailboxIds/old-mailbox") == std::string::npos);
    CHECK(fixture.methodTransport.setArguments.find("keywords") == std::string::npos);
    const auto progress = fixture.progress(admission.operationId);
    CHECK(progress.createdItems == 0);
    CHECK(progress.reusedItems == 1);
    CHECK(progress.failedItems == 0);
}

TEST_CASE("mail import service retries transient failures without a reachability transition",
          "[app][mail-import][service][retry]")
{
    Fixture fixture;
    fixture.methodTransport.transientStateFailuresRemaining = 1;
    const auto admission = fixture.start();
    const bool completed = spinUntil(
        [&]
        { return fixture.operation(admission.operationId).status == MailImportStatus::Complete; });
    if (!completed)
        fixture.captureState(admission);
    REQUIRE(completed);

    CHECK(fixture.methodTransport.stateCalls == 2);
    CHECK(fixture.methodTransport.importCalls == 1);
    CHECK(fixture.resourceTransport.uploadCalls == 1);
}

TEST_CASE("mail import service reconciles ambiguous Email import without replaying creation",
          "[app][mail-import][service][ambiguity]")
{
    Fixture fixture;
    fixture.methodTransport.behavior = ImportBehavior::DispatchedFailure;
    const auto admission = fixture.start();
    const bool blocked = spinUntil(
        [&]
        {
            return fixture.operation(admission.operationId).status ==
                   MailImportStatus::BlockedUnknown;
        });
    if (!blocked)
        fixture.captureState(admission);
    REQUIRE(blocked);
    CHECK(fixture.methodTransport.importCalls == 1);

    fixture.methodTransport.reconciliationCreatedIds = {"candidate-email"};
    REQUIRE_FALSE(fixture.scheduler.retry(admission.jobId).has_value());
    const bool reconciled = spinUntil(
        [&]
        { return fixture.operation(admission.operationId).status == MailImportStatus::Complete; });
    if (!reconciled)
        fixture.captureState(admission);
    REQUIRE(reconciled);

    CHECK(fixture.methodTransport.importCalls == 1);
    CHECK(fixture.methodTransport.changesCalls == 1);
    CHECK(fixture.methodTransport.candidateGetCalls == 1);
    const auto progress = fixture.progress(admission.operationId);
    CHECK(progress.createdItems == 1);
    CHECK(progress.unknownItems == 0);
}
