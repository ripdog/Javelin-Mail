#include "jmap/identity/IdentityService.h"

#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/SessionParser.h"
#include "jmap/cache/IdentityRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/identity/IdentityMutationJournal.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <utility>
#include <vector>

namespace
{
    class FakeMethodTransport final : public javelin::jmap::api::JmapMethodTransport
    {
      public:
        std::vector<javelin::jmap::api::JmapMethodRequest> requests;
        std::vector<javelin::jmap::api::JmapMethodTransportResult> results;
        std::function<javelin::jmap::api::JmapMethodTransportResult(
            const javelin::jmap::api::JmapMethodRequest&)>
            responder;
        std::function<void()> beforeReturn;

        QCoro::Task<javelin::jmap::api::JmapMethodTransportResult>
        call(javelin::jmap::api::JmapMethodRequest request) override
        {
            if (request.dispatched)
                request.dispatched();
            requests.push_back(std::move(request));
            javelin::jmap::api::JmapMethodTransportResult result;
            if (responder)
                result = responder(requests.back());
            else
            {
                REQUIRE_FALSE(results.empty());
                result = std::move(results.front());
                results.erase(results.begin());
            }
            if (beforeReturn)
                beforeReturn();
            co_return result;
        }
    };

    void ensureApplication()
    {
        if (QCoreApplication::instance() != nullptr)
            return;
        static int argc = 1;
        static char name[] = "identity-service-test";
        static char* argv[] = {name, nullptr};
        static QCoreApplication application(argc, argv);
        Q_UNUSED(application);
    }

    [[nodiscard]] QByteArray sessionResponse()
    {
        return QByteArray{
            R"({"username":"alice@example.test","apiUrl":"https://example.test/jmap","downloadUrl":"https://example.test/download/{accountId}/{blobId}/{name}?accept={type}","uploadUrl":"https://example.test/upload/{accountId}","state":"s1","capabilities":{"urn:ietf:params:jmap:core":{"maxSizeRequest":1000000,"maxConcurrentRequests":8,"maxCallsInRequest":16,"maxObjectsInGet":500,"maxObjectsInSet":500},"urn:ietf:params:jmap:mail":{},"urn:ietf:params:jmap:submission":{}},"accounts":{"mail-account":{"name":"Personal","isPersonal":true,"isReadOnly":false,"accountCapabilities":{"urn:ietf:params:jmap:mail":{},"urn:ietf:params:jmap:submission":{}}}},"primaryAccounts":{"urn:ietf:params:jmap:mail":"mail-account","urn:ietf:params:jmap:submission":"mail-account"}})"};
    }

    [[nodiscard]] QByteArray multiAccountSessionResponse()
    {
        return QByteArray{
            R"({"username":"alice@example.test","apiUrl":"https://example.test/jmap","downloadUrl":"https://example.test/download/{accountId}/{blobId}/{name}?accept={type}","uploadUrl":"https://example.test/upload/{accountId}","state":"s1","capabilities":{"urn:ietf:params:jmap:core":{"maxSizeRequest":1000000,"maxConcurrentRequests":8,"maxCallsInRequest":16,"maxObjectsInGet":500,"maxObjectsInSet":500},"urn:ietf:params:jmap:mail":{},"urn:ietf:params:jmap:submission":{}},"accounts":{"owner-account":{"name":"Mail","isPersonal":true,"isReadOnly":false,"accountCapabilities":{"urn:ietf:params:jmap:mail":{}}},"identity-account":{"name":"Sending","isPersonal":false,"isReadOnly":false,"accountCapabilities":{"urn:ietf:params:jmap:submission":{}}}},"primaryAccounts":{"urn:ietf:params:jmap:mail":"owner-account","urn:ietf:params:jmap:submission":"identity-account"}})"};
    }

    [[nodiscard]] javelin::jmap::LiveConnectionSettings settings()
    {
        return {.sessionUrl = "https://example.test/session",
                .loginEmail = "alice@example.test",
                .apiKey = "secret"};
    }

    [[nodiscard]] javelin::jmap::cache::DatabaseConnection
    databaseWithSession(QTemporaryDir& directory, const QString& connectionName,
                        const std::string_view ownerAccountId, const QByteArray& response)
    {
        auto opened = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = connectionName,
            .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
        });
        REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
        auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
        const auto parsed = javelin::jmap::api::parseSession(response.toStdString());
        REQUIRE(parsed.ok());
        javelin::jmap::cache::SessionRepository sessions{connection};
        REQUIRE_FALSE(sessions.replace(ownerAccountId, *parsed.session).has_value());
        return connection;
    }

    [[nodiscard]] javelin::jmap::cache::DatabaseConnection database(QTemporaryDir& directory,
                                                                    const QString& connectionName)
    {
        return databaseWithSession(directory, connectionName, "mail-account", sessionResponse());
    }

    [[nodiscard]] javelin::jmap::domain::Identity existingIdentity(std::string signature = "Old")
    {
        return {
            .id = "identity-1",
            .name = "Alice",
            .email = "alice@example.test",
            .replyTo = {},
            .bcc = {},
            .textSignature = signature,
            .htmlSignature = "<p>" + signature + "</p>",
            .mayDelete = true,
        };
    }

    [[nodiscard]] javelin::jmap::api::ResponseEnvelope updateAcceptedResponse()
    {
        return {
            .methodResponses =
                {
                    {.name = "Identity/set",
                     .arguments =
                         R"({"accountId":"mail-account","oldState":"i1","newState":"i2","updated":{"identity-1":null},"notUpdated":{}})",
                     .callId = "identity-set"},
                    {.name = "Identity/get",
                     .arguments =
                         R"({"accountId":"mail-account","state":"i2","list":[{"id":"identity-1","name":"Alice Work","email":"alice@example.test","replyTo":null,"bcc":null,"textSignature":"New","htmlSignature":"<p>New</p>","mayDelete":true}],"notFound":[]})",
                     .callId = "identity-after-set"},
                },
            .createdIds = std::nullopt,
            .sessionState = "s2",
        };
    }
} // namespace

TEST_CASE("identity refresh uses owner credentials for a secondary submission account",
          "[jmap][identity][service][multi-account]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto connection = databaseWithSession(directory, QStringLiteral("identity-secondary-service"),
                                          "owner-account", multiAccountSessionResponse());
    FakeMethodTransport transport;
    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "Identity/get",
              .arguments =
                  R"({"accountId":"identity-account","state":"i1","list":[{"id":"identity-1","name":"Alice","email":"alice@example.test","replyTo":null,"bcc":null,"textSignature":"Secondary","htmlSignature":"<p>Secondary</p>","mayDelete":true}],"notFound":[]})",
              .callId = "identity-refresh"}},
        .createdIds = std::nullopt,
        .sessionState = "s2"});

    javelin::jmap::identity::IdentityService service{connection, transport};
    const auto result =
        QCoro::waitFor(service.refresh(settings(), "owner-account", "identity-account"));

    REQUIRE(std::holds_alternative<javelin::jmap::identity::IdentitySnapshot>(result));
    REQUIRE(transport.requests.size() == 1);
    CHECK(transport.requests.front().accountId == "owner-account");
    REQUIRE(transport.requests.front().envelope.methodCalls.size() == 1);
    CHECK(transport.requests.front().envelope.methodCalls.front().arguments.find(
              "\"accountId\":\"identity-account\"") != std::string::npos);
    javelin::jmap::cache::IdentityRepository repository{connection};
    const auto identities = repository.listByAccount("identity-account");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::domain::Identity>>(identities));
    CHECK(std::get<std::vector<javelin::jmap::domain::Identity>>(identities).size() == 1);
}

TEST_CASE("identity refresh stores duplicate-address server identities and state",
          "[jmap][identity][service]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto connection = database(directory, QStringLiteral("identity-refresh-service"));
    FakeMethodTransport transport;
    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "Identity/get",
              .arguments =
                  R"({"accountId":"mail-account","state":"i1","list":[{"id":"identity-1","name":"Alice","email":"alice@example.test","replyTo":null,"bcc":null,"textSignature":"Personal","htmlSignature":"<p>Personal</p>","mayDelete":false},{"id":"identity-2","name":"Alice","email":"alice@example.test","replyTo":null,"bcc":null,"textSignature":"Work","htmlSignature":"<p>Work</p>","mayDelete":true}],"notFound":[]})",
              .callId = "identity-refresh"}},
        .createdIds = std::nullopt,
        .sessionState = "s2"});

    javelin::jmap::identity::IdentityService service{connection, transport};
    const auto result = QCoro::waitFor(service.refresh(settings(), "mail-account"));

    REQUIRE(std::holds_alternative<javelin::jmap::identity::IdentitySnapshot>(result));
    const auto& snapshot = std::get<javelin::jmap::identity::IdentitySnapshot>(result);
    REQUIRE(snapshot.identities.size() == 2);
    CHECK(snapshot.identities.at(0).email == snapshot.identities.at(1).email);
    javelin::jmap::cache::IdentityRepository repository{connection};
    const auto state = repository.state("mail-account");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(state));
    CHECK(std::get<std::optional<std::string>>(state) == std::optional<std::string>{"i1"});
}

TEST_CASE("identity refresh advances multi-page changes atomically", "[jmap][identity][service]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto connection = database(directory, QStringLiteral("identity-incremental-service"));
    javelin::jmap::cache::IdentityRepository repository{connection};
    auto identityOne = existingIdentity("Old");
    auto identityThree = existingIdentity("Delete three");
    identityThree.id = "identity-3";
    identityThree.email = "three@example.test";
    auto identityFour = existingIdentity("Delete four");
    identityFour.id = "identity-4";
    identityFour.email = "four@example.test";
    REQUIRE_FALSE(
        repository.replaceAll("mail-account", {identityOne, identityThree, identityFour}, "i1")
            .has_value());

    FakeMethodTransport transport;
    transport.results = {
        javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {{.name = "Identity/changes",
                  .arguments =
                      R"({"accountId":"mail-account","oldState":"i1","newState":"i2","hasMoreChanges":true,"created":["identity-2"],"updated":["identity-1"],"destroyed":["identity-3"]})",
                  .callId = "identity-changes"}},
            .createdIds = std::nullopt,
            .sessionState = "s2"},
        javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {{.name = "Identity/changes",
                  .arguments =
                      R"({"accountId":"mail-account","oldState":"i2","newState":"i3","hasMoreChanges":false,"created":["identity-5"],"updated":[],"destroyed":["identity-4"]})",
                  .callId = "identity-changes"}},
            .createdIds = std::nullopt,
            .sessionState = "s3"},
        javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {{.name = "Identity/get",
                  .arguments =
                      R"({"accountId":"mail-account","state":"i4","list":[{"id":"identity-2","name":"Alice Personal","email":"alice@example.test","replyTo":null,"bcc":null,"textSignature":"Personal","htmlSignature":"<p>Personal</p>","mayDelete":true},{"id":"identity-1","name":"Alice Work","email":"alice@example.test","replyTo":null,"bcc":null,"textSignature":"Updated","htmlSignature":"<p>Updated</p>","mayDelete":true},{"id":"identity-5","name":"Alice Other","email":"other@example.test","replyTo":null,"bcc":null,"textSignature":"Other","htmlSignature":"<p>Other</p>","mayDelete":true}],"notFound":[]})",
                  .callId = "identity-incremental-get"}},
            .createdIds = std::nullopt,
            .sessionState = "s4"},
    };

    javelin::jmap::identity::IdentityService service{connection, transport};
    const auto result = QCoro::waitFor(service.refresh(settings(), "mail-account"));

    REQUIRE(std::holds_alternative<javelin::jmap::identity::IdentitySnapshot>(result));
    const auto& snapshot = std::get<javelin::jmap::identity::IdentitySnapshot>(result);
    REQUIRE(snapshot.identities.size() == 3);
    CHECK(std::ranges::find(snapshot.identities, "identity-3",
                            &javelin::jmap::domain::Identity::id) == snapshot.identities.end());
    CHECK(std::ranges::find(snapshot.identities, "identity-4",
                            &javelin::jmap::domain::Identity::id) == snapshot.identities.end());
    const auto updated =
        std::ranges::find(snapshot.identities, "identity-1", &javelin::jmap::domain::Identity::id);
    REQUIRE(updated != snapshot.identities.end());
    CHECK(updated->textSignature == std::optional<std::string>{"Updated"});
    const auto state = repository.state("mail-account");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(state));
    CHECK(std::get<std::optional<std::string>>(state) == std::optional<std::string>{"i3"});
    REQUIRE(transport.requests.size() == 3);
    CHECK(transport.requests.at(0).envelope.methodCalls.front().name == "Identity/changes");
    CHECK(transport.requests.at(1).envelope.methodCalls.front().name == "Identity/changes");
    CHECK(transport.requests.at(2).envelope.methodCalls.front().name == "Identity/get");
}

TEST_CASE("identity refresh falls back to full replacement when changes cannot advance",
          "[jmap][identity][service]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto connection = database(directory, QStringLiteral("identity-fallback-service"));
    javelin::jmap::cache::IdentityRepository repository{connection};
    REQUIRE_FALSE(repository.replaceAll("mail-account", {existingIdentity()}, "stale").has_value());
    FakeMethodTransport transport;
    transport.results = {
        javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {{.name = "error",
                  .arguments = R"({"type":"cannotCalculateChanges","description":"state too old"})",
                  .callId = "identity-changes"}},
            .createdIds = std::nullopt,
            .sessionState = "s2"},
        javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {{.name = "Identity/get",
                  .arguments =
                      R"({"accountId":"mail-account","state":"fresh","list":[{"id":"identity-7","name":"Fresh","email":"fresh@example.test","replyTo":null,"bcc":null,"textSignature":"Fresh","htmlSignature":"<p>Fresh</p>","mayDelete":false}],"notFound":[]})",
                  .callId = "identity-refresh"}},
            .createdIds = std::nullopt,
            .sessionState = "s3"},
    };

    javelin::jmap::identity::IdentityService service{connection, transport};
    const auto result = QCoro::waitFor(service.refresh(settings(), "mail-account"));

    REQUIRE(std::holds_alternative<javelin::jmap::identity::IdentitySnapshot>(result));
    const auto& snapshot = std::get<javelin::jmap::identity::IdentitySnapshot>(result);
    REQUIRE(snapshot.identities.size() == 1);
    CHECK(snapshot.identities.front().id == "identity-7");
    const auto state = repository.state("mail-account");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(state));
    CHECK(std::get<std::optional<std::string>>(state) == std::optional<std::string>{"fresh"});
    REQUIRE(transport.requests.size() == 2);
    CHECK(transport.requests.at(0).envelope.methodCalls.front().name == "Identity/changes");
    CHECK(transport.requests.at(1).envelope.methodCalls.front().name == "Identity/get");
}

TEST_CASE("identity updates project before dispatch completion and accept canonical server state",
          "[jmap][identity][service]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto connection = database(directory, QStringLiteral("identity-update-service"));
    javelin::jmap::cache::IdentityRepository repository{connection};
    REQUIRE_FALSE(repository.replaceAll("mail-account", {existingIdentity()}, "i1").has_value());
    FakeMethodTransport transport;
    transport.results.push_back(updateAcceptedResponse());
    bool projectionPublished = false;
    transport.beforeReturn = [&repository, &projectionPublished]
    {
        CHECK(projectionPublished);
        const auto projected = repository.find("mail-account", "identity-1");
        REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Identity>>(projected));
        const auto& identity = std::get<std::optional<javelin::jmap::domain::Identity>>(projected);
        REQUIRE(identity.has_value());
        CHECK(identity->textSignature == std::optional<std::string>{"New"});
    };

    auto updated = existingIdentity("New");
    updated.name = "Alice Work";
    javelin::jmap::identity::IdentityService service{connection, transport};
    const auto result =
        QCoro::waitFor(service.save(settings(), "mail-account", updated, std::nullopt,
                                    [&projectionPublished] { projectionPublished = true; }));

    REQUIRE(std::holds_alternative<javelin::jmap::domain::Identity>(result));
    CHECK(std::get<javelin::jmap::domain::Identity>(result).name == "Alice Work");
    REQUIRE(transport.requests.size() == 1);
    REQUIRE(transport.requests.front().envelope.methodCalls.size() == 2);
    CHECK(transport.requests.front().envelope.methodCalls.at(0).name == "Identity/set");
    CHECK(transport.requests.front().envelope.methodCalls.at(1).name == "Identity/get");
    CHECK(transport.requests.front().envelope.methodCalls.at(0).arguments.find("\"email\"") ==
          std::string::npos);

    javelin::jmap::identity::IdentityMutationJournal journal{connection, repository};
    const auto active = journal.listActive("mail-account");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::identity::IdentityMutationRecord>>(
        active));
    CHECK(std::get<std::vector<javelin::jmap::identity::IdentityMutationRecord>>(active).empty());
}

TEST_CASE("rejected identity updates restore the previous cache projection",
          "[jmap][identity][service]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto connection = database(directory, QStringLiteral("identity-rejected-service"));
    javelin::jmap::cache::IdentityRepository repository{connection};
    REQUIRE_FALSE(repository.replaceAll("mail-account", {existingIdentity()}, "i1").has_value());
    FakeMethodTransport transport;
    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {
                {.name = "Identity/set",
                 .arguments =
                     R"({"accountId":"mail-account","oldState":"i1","newState":"i1","notUpdated":{"identity-1":{"type":"forbidden","description":"Managed by administrator"}}})",
                 .callId = "identity-set"},
                {.name = "Identity/get",
                 .arguments =
                     R"({"accountId":"mail-account","state":"i1","list":[{"id":"identity-1","name":"Alice","email":"alice@example.test","replyTo":null,"bcc":null,"textSignature":"Old","htmlSignature":"<p>Old</p>","mayDelete":true}],"notFound":[]})",
                 .callId = "identity-after-set"},
            },
        .createdIds = std::nullopt,
        .sessionState = "s2"});

    auto updated = existingIdentity("Rejected");
    javelin::jmap::identity::IdentityService service{connection, transport};
    const auto result = QCoro::waitFor(service.save(settings(), "mail-account", updated));

    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
    CHECK(std::get<javelin::jmap::OperationError>(result).code ==
          javelin::jmap::OperationErrorCode::PermissionDenied);
    const auto restored = repository.find("mail-account", "identity-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Identity>>(restored));
    const auto& identity = std::get<std::optional<javelin::jmap::domain::Identity>>(restored);
    REQUIRE(identity.has_value());
    CHECK(identity->textSignature == std::optional<std::string>{"Old"});
}

TEST_CASE("identity creates remain pending without a fake server id until accepted",
          "[jmap][identity][service]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto connection = database(directory, QStringLiteral("identity-create-service"));
    javelin::jmap::cache::IdentityRepository repository{connection};
    REQUIRE_FALSE(repository.replaceAll("mail-account", {existingIdentity()}, "i1").has_value());
    FakeMethodTransport transport;
    transport.responder = [](const javelin::jmap::api::JmapMethodRequest& request)
    {
        const auto setArguments = request.envelope.methodCalls.front().arguments;
        const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(setArguments));
        REQUIRE(document.isObject());
        const auto create = document.object().value(QStringLiteral("create")).toObject();
        REQUIRE(create.size() == 1);
        const auto creationId = create.begin().key();
        return javelin::jmap::api::JmapMethodTransportResult{javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {
                    {.name = "Identity/set",
                     .arguments =
                         QStringLiteral(
                             R"({"accountId":"mail-account","oldState":"i1","newState":"i2","created":{"%1":{"id":"identity-2"}},"notCreated":{}})")
                             .arg(creationId)
                             .toStdString(),
                     .callId = "identity-set"},
                    {.name = "Identity/get",
                     .arguments =
                         R"({"accountId":"mail-account","state":"i2","list":[{"id":"identity-1","name":"Alice","email":"alice@example.test","replyTo":null,"bcc":null,"textSignature":"Old","htmlSignature":"<p>Old</p>","mayDelete":true},{"id":"identity-2","name":"Alice Work","email":"alice@example.test","replyTo":null,"bcc":null,"textSignature":"Work","htmlSignature":"<p>Work</p>","mayDelete":true}],"notFound":[]})",
                     .callId = "identity-after-set"},
                },
            .createdIds = std::nullopt,
            .sessionState = "s2",
        }};
    };
    transport.beforeReturn = [&repository]
    {
        const auto confirmed = repository.listByAccount("mail-account");
        REQUIRE(std::holds_alternative<std::vector<javelin::jmap::domain::Identity>>(confirmed));
        CHECK(std::get<std::vector<javelin::jmap::domain::Identity>>(confirmed).size() == 1);
        const auto pending = repository.listPendingCreates("mail-account");
        REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::PendingIdentityCreate>>(
            pending));
        REQUIRE(
            std::get<std::vector<javelin::jmap::cache::PendingIdentityCreate>>(pending).size() ==
            1);
        CHECK(std::get<std::vector<javelin::jmap::cache::PendingIdentityCreate>>(pending)
                  .front()
                  .identity.id.empty());
    };

    auto created = existingIdentity("Work");
    created.id.clear();
    created.name = "Alice Work";
    javelin::jmap::identity::IdentityService service{connection, transport};
    const auto result = QCoro::waitFor(service.save(settings(), "mail-account", created));

    REQUIRE(std::holds_alternative<javelin::jmap::domain::Identity>(result));
    CHECK(std::get<javelin::jmap::domain::Identity>(result).id == "identity-2");
    const auto pending = repository.listPendingCreates("mail-account");
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::cache::PendingIdentityCreate>>(pending));
    CHECK(std::get<std::vector<javelin::jmap::cache::PendingIdentityCreate>>(pending).empty());
}

TEST_CASE("identity deletes project before dispatch completion and accept server state",
          "[jmap][identity][service]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto connection = database(directory, QStringLiteral("identity-delete-service"));
    javelin::jmap::cache::IdentityRepository repository{connection};
    REQUIRE_FALSE(repository.replaceAll("mail-account", {existingIdentity()}, "i1").has_value());
    FakeMethodTransport transport;
    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {
                {.name = "Identity/set",
                 .arguments =
                     R"({"accountId":"mail-account","oldState":"i1","newState":"i2","destroyed":["identity-1"],"notDestroyed":{}})",
                 .callId = "identity-set"},
                {.name = "Identity/get",
                 .arguments =
                     R"({"accountId":"mail-account","state":"i2","list":[],"notFound":[]})",
                 .callId = "identity-after-set"},
            },
        .createdIds = std::nullopt,
        .sessionState = "s2"});
    transport.beforeReturn = [&repository]
    {
        const auto projected = repository.find("mail-account", "identity-1");
        REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Identity>>(projected));
        CHECK_FALSE(
            std::get<std::optional<javelin::jmap::domain::Identity>>(projected).has_value());
    };

    javelin::jmap::identity::IdentityService service{connection, transport};
    const auto result = QCoro::waitFor(service.remove(settings(), "mail-account", "identity-1"));

    REQUIRE(std::holds_alternative<std::monostate>(result));
    const auto remaining = repository.listByAccount("mail-account");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::domain::Identity>>(remaining));
    CHECK(std::get<std::vector<javelin::jmap::domain::Identity>>(remaining).empty());
}

TEST_CASE("non-deletable identities are rejected locally without dispatch",
          "[jmap][identity][service]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto connection = database(directory, QStringLiteral("identity-delete-forbidden-service"));
    auto identity = existingIdentity();
    identity.mayDelete = false;
    javelin::jmap::cache::IdentityRepository repository{connection};
    REQUIRE_FALSE(repository.replaceAll("mail-account", {identity}, "i1").has_value());
    FakeMethodTransport transport;
    javelin::jmap::identity::IdentityService service{connection, transport};

    const auto result = QCoro::waitFor(service.remove(settings(), "mail-account", "identity-1"));

    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
    CHECK(std::get<javelin::jmap::OperationError>(result).code ==
          javelin::jmap::OperationErrorCode::PermissionDenied);
    CHECK(transport.requests.empty());
}

TEST_CASE("ambiguous identity updates stay projected and recover from a matching refresh",
          "[jmap][identity][service]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto connection = database(directory, QStringLiteral("identity-unknown-service"));
    javelin::jmap::cache::IdentityRepository repository{connection};
    REQUIRE_FALSE(repository.replaceAll("mail-account", {existingIdentity()}, "i1").has_value());
    FakeMethodTransport transport;
    transport.results.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
        .message = "connection reset after dispatch",
    });
    auto updated = existingIdentity("Recovered");
    javelin::jmap::identity::IdentityService service{connection, transport};
    const auto failed = QCoro::waitFor(service.save(settings(), "mail-account", updated));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(failed));

    auto projected = repository.find("mail-account", "identity-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Identity>>(projected));
    CHECK(std::get<std::optional<javelin::jmap::domain::Identity>>(projected)->textSignature ==
          std::optional<std::string>{"Recovered"});

    transport.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "Identity/get",
              .arguments =
                  R"({"accountId":"mail-account","state":"i2","list":[{"id":"identity-1","name":"Alice","email":"alice@example.test","replyTo":null,"bcc":null,"textSignature":"Recovered","htmlSignature":"<p>Recovered</p>","mayDelete":true}],"notFound":[]})",
              .callId = "identity-refresh"}},
        .createdIds = std::nullopt,
        .sessionState = "s3"});
    const auto refreshed = QCoro::waitFor(service.refresh(settings(), "mail-account"));
    REQUIRE(std::holds_alternative<javelin::jmap::identity::IdentitySnapshot>(refreshed));
    javelin::jmap::identity::IdentityMutationJournal journal{connection, repository};
    const auto active = journal.listActive("mail-account");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::identity::IdentityMutationRecord>>(
        active));
    CHECK(std::get<std::vector<javelin::jmap::identity::IdentityMutationRecord>>(active).empty());
}
