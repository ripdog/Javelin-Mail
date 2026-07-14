#include "jmap/sieve/SieveService.h"

#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/Transport.h"

#include <QCoroTask>

#include <QCoreApplication>

#include <catch2/catch_test_macros.hpp>

#include <utility>
#include <vector>

namespace
{
    class FakeResourceTransport final : public javelin::jmap::api::AbstractTransport
    {
      public:
        std::vector<javelin::jmap::api::HttpRequest> requests;
        std::vector<javelin::jmap::api::TransportResult> results;

        QCoro::Task<javelin::jmap::api::TransportResult>
        send(javelin::jmap::api::HttpRequest request) override
        {
            requests.push_back(std::move(request));
            REQUIRE_FALSE(results.empty());
            auto result = std::move(results.front());
            results.erase(results.begin());
            co_return result;
        }
    };

    class FakeMethodTransport final : public javelin::jmap::api::JmapMethodTransport
    {
      public:
        std::vector<javelin::jmap::api::JmapMethodRequest> requests;
        std::vector<javelin::jmap::api::JmapMethodTransportResult> results;

        QCoro::Task<javelin::jmap::api::JmapMethodTransportResult>
        call(javelin::jmap::api::JmapMethodRequest request) override
        {
            requests.push_back(std::move(request));
            REQUIRE_FALSE(results.empty());
            auto result = std::move(results.front());
            results.erase(results.begin());
            co_return result;
        }
    };

    void ensureApplication()
    {
        if (QCoreApplication::instance() != nullptr)
            return;
        static int argc = 1;
        static char name[] = "sieve-service-test";
        static char* argv[] = {name, nullptr};
        static QCoreApplication application(argc, argv);
        Q_UNUSED(application);
    }

    [[nodiscard]] QByteArray sessionResponse()
    {
        return QByteArray{
            R"({"username":"alice@example.test","apiUrl":"https://example.test/jmap","downloadUrl":"https://example.test/download/{accountId}/{blobId}/{name}?accept={type}","uploadUrl":"https://example.test/upload/{accountId}","state":"s1","capabilities":{"urn:ietf:params:jmap:core":{},"urn:ietf:params:jmap:sieve":{}},"accounts":{"sieve-account":{"name":"Personal","isPersonal":true,"isReadOnly":false,"accountCapabilities":{"urn:ietf:params:jmap:sieve":{}}}},"primaryAccounts":{"urn:ietf:params:jmap:sieve":"sieve-account"}})"};
    }

    [[nodiscard]] javelin::jmap::LiveConnectionSettings settings()
    {
        return {.sessionUrl = "https://example.test/session",
                .loginEmail = "alice@example.test",
                .apiKey = "secret"};
    }
} // namespace

TEST_CASE("sieve save never updates a script rejected by server validation",
          "[jmap][sieve][service]")
{
    ensureApplication();
    FakeResourceTransport resources;
    resources.results = {
        javelin::jmap::api::HttpResponse{.statusCode = 200, .body = sessionResponse()},
        javelin::jmap::api::HttpResponse{
            .statusCode = 201,
            .body =
                QByteArray{
                    R"({"accountId":"sieve-account","blobId":"blob-new","type":"application/sieve","size":13})"}},
    };
    FakeMethodTransport methods;
    methods.results.push_back(javelin::jmap::api::ResponseEnvelope{
        .methodResponses =
            {{.name = "SieveScript/validate",
              .arguments =
                  R"({"accountId":"sieve-account","error":{"type":"invalidSieve","description":"line 1: missing semicolon"}})",
              .callId = "sieve-validate"}},
        .createdIds = std::nullopt,
        .sessionState = "s2"});

    javelin::jmap::sieve::SieveService service{resources, methods};
    const auto result = QCoro::waitFor(
        service.save(settings(), "owner", {.id = "script-1", .name = "main", .blobId = "blob-old"},
                     QByteArrayLiteral("discard;")));

    REQUIRE(std::holds_alternative<javelin::jmap::sieve::SieveServiceError>(result));
    const auto& error = std::get<javelin::jmap::sieve::SieveServiceError>(result);
    CHECK(error.code == javelin::jmap::sieve::SieveServiceErrorCode::InvalidScript);
    CHECK(error.message == QStringLiteral("line 1: missing semicolon"));
    REQUIRE(methods.requests.size() == 1);
    CHECK(methods.requests.front().envelope.methodCalls.front().name == "SieveScript/validate");
    REQUIRE(resources.requests.size() == 2);
    CHECK(resources.requests.back().body == QByteArrayLiteral("discard;"));
    CHECK(resources.requests.back().headers.back().value == QByteArrayLiteral("application/sieve"));
}

TEST_CASE("sieve save updates with the exact blob accepted by validation", "[jmap][sieve][service]")
{
    ensureApplication();
    FakeResourceTransport resources;
    resources.results = {
        javelin::jmap::api::HttpResponse{.statusCode = 200, .body = sessionResponse()},
        javelin::jmap::api::HttpResponse{
            .statusCode = 201,
            .body =
                QByteArray{
                    R"({"accountId":"sieve-account","blobId":"blob-validated","type":"application/sieve","size":6})"}},
    };
    FakeMethodTransport methods;
    methods.results = {
        javelin::jmap::api::ResponseEnvelope{
            .methodResponses = {{.name = "SieveScript/validate",
                                 .arguments = R"({"accountId":"sieve-account","error":null})",
                                 .callId = "sieve-validate"}},
            .createdIds = std::nullopt,
            .sessionState = "s2"},
        javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {{.name = "SieveScript/set",
                  .arguments =
                      R"({"accountId":"sieve-account","oldState":"a","newState":"b","updated":{"script-1":null},"notUpdated":{}})",
                  .callId = "sieve-save"}},
            .createdIds = std::nullopt,
            .sessionState = "s3"},
    };

    javelin::jmap::sieve::SieveService service{resources, methods};
    const auto result = QCoro::waitFor(
        service.save(settings(), "owner", {.id = "script-1", .name = "main", .blobId = "blob-old"},
                     QByteArrayLiteral("keep;")));

    REQUIRE(std::holds_alternative<javelin::jmap::sieve::SieveScript>(result));
    CHECK(std::get<javelin::jmap::sieve::SieveScript>(result).blobId == "blob-validated");
    REQUIRE(methods.requests.size() == 2);
    CHECK(methods.requests[0].envelope.methodCalls.front().name == "SieveScript/validate");
    CHECK(methods.requests[1].envelope.methodCalls.front().name == "SieveScript/set");
    CHECK(methods.requests[1].envelope.methodCalls.front().arguments.find("blob-validated") !=
          std::string::npos);
}

TEST_CASE("new sieve scripts are validated before creation", "[jmap][sieve][service]")
{
    ensureApplication();
    FakeResourceTransport resources;
    resources.results = {
        javelin::jmap::api::HttpResponse{.statusCode = 200, .body = sessionResponse()},
        javelin::jmap::api::HttpResponse{
            .statusCode = 201,
            .body =
                QByteArray{
                    R"({"accountId":"sieve-account","blobId":"blob-new","type":"application/sieve","size":6})"}},
    };
    FakeMethodTransport methods;
    methods.results = {
        javelin::jmap::api::ResponseEnvelope{
            .methodResponses = {{.name = "SieveScript/validate",
                                 .arguments = R"({"accountId":"sieve-account","error":null})",
                                 .callId = "sieve-validate"}},
            .createdIds = std::nullopt,
            .sessionState = "s2"},
        javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {{.name = "SieveScript/set",
                  .arguments =
                      R"({"accountId":"sieve-account","oldState":"a","newState":"b","created":{"new-script":{"id":"script-2"}},"notCreated":{}})",
                  .callId = "sieve-save"}},
            .createdIds = std::nullopt,
            .sessionState = "s3"},
    };

    javelin::jmap::sieve::SieveService service{resources, methods};
    const auto result = QCoro::waitFor(service.save(settings(), "owner",
                                                    {.id = {}, .name = "vacation", .blobId = {}},
                                                    QByteArrayLiteral("keep;")));

    REQUIRE(std::holds_alternative<javelin::jmap::sieve::SieveScript>(result));
    const auto& created = std::get<javelin::jmap::sieve::SieveScript>(result);
    CHECK(created.id == "script-2");
    CHECK(created.name == "vacation");
    CHECK(created.blobId == "blob-new");
    REQUIRE(methods.requests.size() == 2);
    CHECK(methods.requests[0].envelope.methodCalls.front().name == "SieveScript/validate");
    const auto& creation = methods.requests[1].envelope.methodCalls.front();
    CHECK(creation.name == "SieveScript/set");
    CHECK(creation.arguments.find("vacation") != std::string::npos);
    CHECK(creation.arguments.find("blob-new") != std::string::npos);
}

TEST_CASE("deleting an active sieve script deactivates it in a separate call",
          "[jmap][sieve][service]")
{
    ensureApplication();
    FakeResourceTransport resources;
    resources.results = {
        javelin::jmap::api::HttpResponse{.statusCode = 200, .body = sessionResponse()},
    };
    FakeMethodTransport methods;
    methods.results = {
        javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {{.name = "SieveScript/set",
                  .arguments =
                      R"({"accountId":"sieve-account","oldState":"a","newState":"b","updated":{"script-1":{"isActive":false}}})",
                  .callId = "sieve-deactivate"}},
            .createdIds = std::nullopt,
            .sessionState = "s2"},
        javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {{.name = "SieveScript/set",
                  .arguments =
                      R"({"accountId":"sieve-account","oldState":"b","newState":"c","destroyed":["script-1"],"notDestroyed":{}})",
                  .callId = "sieve-delete"}},
            .createdIds = std::nullopt,
            .sessionState = "s3"},
    };

    javelin::jmap::sieve::SieveService service{resources, methods};
    const auto result = QCoro::waitFor(
        service.remove(settings(), "owner",
                       {.id = "script-1", .name = "main", .blobId = "blob-1", .isActive = true}));

    REQUIRE(std::holds_alternative<std::monostate>(result));
    REQUIRE(methods.requests.size() == 2);
    const auto& deactivate = methods.requests[0].envelope.methodCalls.front();
    const auto& destroy = methods.requests[1].envelope.methodCalls.front();
    CHECK(deactivate.callId == "sieve-deactivate");
    CHECK(deactivate.arguments.find("onSuccessDeactivateScript") != std::string::npos);
    CHECK(destroy.callId == "sieve-delete");
    CHECK(destroy.arguments.find("script-1") != std::string::npos);
}

TEST_CASE("sieve scripts can be activated and deactivated", "[jmap][sieve][service]")
{
    ensureApplication();
    for (const bool active : {true, false})
    {
        FakeResourceTransport resources;
        resources.results = {
            javelin::jmap::api::HttpResponse{.statusCode = 200, .body = sessionResponse()},
        };
        FakeMethodTransport methods;
        methods.results = {
            javelin::jmap::api::ResponseEnvelope{
                .methodResponses =
                    {{.name = "SieveScript/set",
                      .arguments =
                          R"({"accountId":"sieve-account","oldState":"a","newState":"b","updated":{"script-1":null}})",
                      .callId = "sieve-active"}},
                .createdIds = std::nullopt,
                .sessionState = "s2"},
            javelin::jmap::api::ResponseEnvelope{
                .methodResponses =
                    {{.name = "SieveScript/get",
                      .arguments =
                          active
                              ? R"({"accountId":"sieve-account","state":"b","list":[{"id":"script-1","name":"main","blobId":"blob-1","isActive":true}],"notFound":[]})"
                              : R"({"accountId":"sieve-account","state":"b","list":[{"id":"script-1","name":"main","blobId":"blob-1","isActive":false}],"notFound":[]})",
                      .callId = "sieve-active-check"}},
                .createdIds = std::nullopt,
                .sessionState = "s2"},
        };

        javelin::jmap::sieve::SieveService service{resources, methods};
        const auto result = QCoro::waitFor(service.setActive(
            settings(), "owner",
            {.id = "script-1", .name = "main", .blobId = "blob-1", .isActive = !active}, active));

        REQUIRE(std::holds_alternative<std::monostate>(result));
        REQUIRE(methods.requests.size() == 2);
        const auto& request = methods.requests.front().envelope.methodCalls.front();
        CHECK(request.callId == "sieve-active");
        CHECK(request.arguments.find(active ? "onSuccessActivateScript"
                                            : "onSuccessDeactivateScript") != std::string::npos);
        const auto& check = methods.requests.back().envelope.methodCalls.front();
        CHECK(check.name == "SieveScript/get");
        CHECK(check.arguments.find("script-1") != std::string::npos);
    }
}
