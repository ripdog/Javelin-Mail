#include "jmap/api/MethodCaller.h"
#include "FixtureReader.h"
#include "jmap/api/BlobUpload.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MethodEnvelope.h"
#include "jmap/api/Transport.h"
#include "jmap/auth/Auth.h"

#include <QCoroTask>

#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace
{

    void ensureApplication()
    {
        if (QCoreApplication::instance() != nullptr)
        {
            return;
        }

        static int argc = 1;
        static char appName[] = "javelin-tests";
        static char* argv[] = {appName, nullptr};
        static QCoreApplication application(argc, argv);
        Q_UNUSED(application);
    }

    [[nodiscard]] const javelin::jmap::api::HttpHeader*
    header(const javelin::jmap::api::HttpRequest& request, const QByteArray& name)
    {
        for (const auto& value : request.headers)
            if (value.name == name)
                return &value;
        return nullptr;
    }

    class FakeTransport final : public javelin::jmap::api::AbstractTransport
    {
      public:
        FakeTransport() : methodTransport(*this)
        {
        }

        javelin::jmap::api::HttpJmapMethodTransport methodTransport;
        javelin::jmap::api::HttpRequest lastRequest;
        std::vector<javelin::jmap::api::HttpRequest> requests;
        std::vector<javelin::jmap::api::TransportResult> queuedResults;

        [[nodiscard]] QCoro::Task<javelin::jmap::api::TransportResult>
        send(javelin::jmap::api::HttpRequest request) override
        {
            lastRequest = request;
            requests.push_back(request);
            if (request.dispatched)
                request.dispatched();
            REQUIRE_FALSE(queuedResults.empty());

            auto result = std::move(queuedResults.front());
            queuedResults.erase(queuedResults.begin());
            co_return result;
        }

        [[nodiscard]] QCoro::Task<javelin::jmap::api::TransportResult>
        sendFromFile(javelin::jmap::api::HttpRequest request, QString filePath) override
        {
            QFile file{filePath};
            REQUIRE(file.open(QIODevice::ReadOnly));
            request.body = file.readAll();
            co_return co_await send(std::move(request));
        }

        [[nodiscard]] QCoro::Task<javelin::jmap::api::TransportResult>
        sendFromDevice(javelin::jmap::api::HttpRequest request, QIODevice& device,
                       const std::uint64_t contentLength) override
        {
            request.body = device.read(static_cast<qint64>(contentLength));
            REQUIRE(request.body.size() == static_cast<qsizetype>(contentLength));
            co_return co_await send(std::move(request));
        }
    };

    class FakeJmapMethodTransport final : public javelin::jmap::api::JmapMethodTransport
    {
      public:
        std::optional<javelin::jmap::api::JmapMethodRequest> request;
        javelin::jmap::api::JmapMethodTransportResult result;

        [[nodiscard]] QCoro::Task<javelin::jmap::api::JmapMethodTransportResult>
        call(javelin::jmap::api::JmapMethodRequest value) override
        {
            request = std::move(value);
            co_return result;
        }
    };

    class ScriptedJmapMethodTransport final : public javelin::jmap::api::JmapMethodTransport
    {
      public:
        std::vector<javelin::jmap::api::JmapMethodRequest> requests;
        std::vector<javelin::jmap::api::JmapMethodTransportResult> results;

        [[nodiscard]] QCoro::Task<javelin::jmap::api::JmapMethodTransportResult>
        call(javelin::jmap::api::JmapMethodRequest request) override
        {
            if (request.dispatched)
                request.dispatched();
            requests.push_back(request);
            REQUIRE_FALSE(results.empty());
            auto result = std::move(results.front());
            results.erase(results.begin());
            co_return result;
        }
    };

    class FakeTokenRefresher final : public javelin::jmap::auth::TokenRefresher
    {
      public:
        mutable std::size_t calls = 0;
        javelin::jmap::auth::TokenRefreshResult result;

        [[nodiscard]] javelin::jmap::auth::TokenRefreshResult
        refresh(const javelin::jmap::auth::AccountCredentials&) const override
        {
            ++calls;
            return result;
        }
    };

    class FakeSecretStore final : public javelin::jmap::auth::SecretStore
    {
      public:
        mutable std::optional<javelin::jmap::auth::OAuthToken> storedToken;
        bool storeResult = true;

        [[nodiscard]] std::optional<javelin::jmap::auth::OAuthToken>
        load(std::string_view) const override
        {
            return storedToken;
        }

        bool store(std::string_view, const javelin::jmap::auth::OAuthToken& token) override
        {
            storedToken = token;
            return storeResult;
        }

        bool clear(std::string_view) override
        {
            storedToken.reset();
            return true;
        }
    };

    [[nodiscard]] javelin::jmap::api::ApiRequestContext makeRequestContext()
    {
        return javelin::jmap::api::ApiRequestContext{
            .credentials =
                {
                    .accountId = "u1",
                    .emailAddress = "alice@example.com",
                    .sessionUrl = "https://mail.example.com/.well-known/jmap",
                    .token =
                        {
                            .accessToken = "access-token",
                            .refreshToken = "refresh-token",
                            .expiry = std::nullopt,
                        },
                },
            .apiUrl = "https://mail.example.com/jmap/api",
            .requestLimits = std::nullopt,
        };
    }

    [[nodiscard]] javelin::jmap::api::RequestEnvelope loadRequestEnvelope()
    {
        const auto parsed = javelin::jmap::api::parseRequestEnvelope(
            javelin::tests::loadFixture("jmap/method/request.json"));
        REQUIRE(parsed.ok());
        REQUIRE(parsed.value.has_value());
        return *parsed.value;
    }

} // namespace

TEST_CASE("method caller rejects cancellation before transport dispatch",
          "[jmap][method][transport][cancellation]")
{
    ensureApplication();
    FakeJmapMethodTransport transport;
    javelin::jmap::api::MethodCaller caller{transport};
    javelin::jmap::api::CancellationSource cancellation;
    cancellation.cancel();

    const auto result = QCoro::waitFor(
        caller.call(makeRequestContext(), loadRequestEnvelope(), cancellation.token()));

    REQUIRE(std::holds_alternative<javelin::jmap::api::TransportError>(result));
    CHECK(std::get<javelin::jmap::api::TransportError>(result).code ==
          javelin::jmap::api::TransportErrorCode::Cancelled);
    CHECK_FALSE(transport.request.has_value());
}

TEST_CASE("method caller rejects envelopes above negotiated method and byte limits",
          "[jmap][method][transport][limits]")
{
    ensureApplication();
    FakeJmapMethodTransport transport;
    javelin::jmap::api::MethodCaller caller{transport};

    auto context = makeRequestContext();
    context.requestLimits = javelin::jmap::api::CoreRequestLimits{
        .maxSizeRequest = 1'000'000,
        .maxConcurrentRequests = 1,
        .maxCallsInRequest = 1,
        .maxObjectsInGet = 100,
        .maxObjectsInSet = 100,
    };
    auto result = QCoro::waitFor(caller.call(context, loadRequestEnvelope()));
    REQUIRE(std::holds_alternative<javelin::jmap::api::ProtocolError>(result));
    CHECK(std::get<javelin::jmap::api::ProtocolError>(result).code ==
          javelin::jmap::api::ProtocolErrorCode::InvalidRequest);
    CHECK_FALSE(transport.request.has_value());

    context.requestLimits->maxCallsInRequest = 100;
    context.requestLimits->maxSizeRequest = 1;
    result = QCoro::waitFor(caller.call(context, loadRequestEnvelope()));
    REQUIRE(std::holds_alternative<javelin::jmap::api::ProtocolError>(result));
    CHECK(std::get<javelin::jmap::api::ProtocolError>(result).code ==
          javelin::jmap::api::ProtocolErrorCode::InvalidRequest);
    CHECK_FALSE(transport.request.has_value());
}

TEST_CASE("method caller posts a typed request envelope and parses the response",
          "[jmap][method][transport]")
{
    ensureApplication();

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArray::fromStdString(javelin::tests::loadFixture("jmap/method/response.json")),
    });

    javelin::jmap::api::MethodCaller methodCaller{transport.methodTransport};
    const auto result =
        QCoro::waitFor(methodCaller.call(makeRequestContext(), loadRequestEnvelope()));

    REQUIRE(std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(result));
    const auto& response = std::get<javelin::jmap::api::ResponseEnvelope>(result);
    CHECK(response.sessionState == "session-state-2");
    CHECK(transport.lastRequest.method == javelin::jmap::api::HttpMethod::Post);
    CHECK(transport.lastRequest.url == QUrl{QStringLiteral("https://mail.example.com/jmap/api")});
    REQUIRE(transport.lastRequest.headers.size() == 3);
    const auto* authorization = header(transport.lastRequest, QByteArrayLiteral("Authorization"));
    REQUIRE(authorization != nullptr);
    CHECK(authorization->value == "Bearer access-token");
    const auto* contentType = header(transport.lastRequest, QByteArrayLiteral("Content-Type"));
    REQUIRE(contentType != nullptr);
    CHECK(contentType->value == "application/json");

    const auto roundTripRequest =
        javelin::jmap::api::parseRequestEnvelope(transport.lastRequest.body.toStdString());
    REQUIRE(roundTripRequest.ok());
    REQUIRE(roundTripRequest.value.has_value());
    CHECK(roundTripRequest.value->methodCalls.size() == 2);
    CHECK(roundTripRequest.value->methodCalls.front().name == "Mailbox/get");
}

TEST_CASE("method caller refreshes expired tokens and persists them when configured",
          "[jmap][method][transport][auth]")
{
    ensureApplication();

    using namespace std::chrono_literals;

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArray::fromStdString(javelin::tests::loadFixture("jmap/method/response.json")),
    });

    FakeTokenRefresher tokenRefresher;
    tokenRefresher.result = javelin::jmap::auth::OAuthToken{
        .accessToken = "refreshed-token",
        .refreshToken = "refresh-token",
        .expiry = javelin::jmap::auth::Clock::now() + 10min,
    };
    FakeSecretStore secretStore;

    auto requestContext = makeRequestContext();
    requestContext.credentials.token.expiry = javelin::jmap::auth::Clock::now();

    javelin::jmap::api::MethodCaller methodCaller{transport.methodTransport, &tokenRefresher,
                                                  &secretStore};
    const auto result = QCoro::waitFor(methodCaller.call(requestContext, loadRequestEnvelope()));

    REQUIRE(std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(result));
    CHECK(tokenRefresher.calls == 1);
    REQUIRE(secretStore.storedToken.has_value());
    CHECK(secretStore.storedToken->accessToken == "refreshed-token");
    const auto* refreshedAuthorization =
        header(transport.lastRequest, QByteArrayLiteral("Authorization"));
    REQUIRE(refreshedAuthorization != nullptr);
    CHECK(refreshedAuthorization->value == "Bearer refreshed-token");
}

TEST_CASE("refreshing HTTP transport retries one definite unauthorized response",
          "[jmap][transport][auth]")
{
    ensureApplication();

    FakeTransport rawTransport;
    rawTransport.queuedResults.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::HttpFailure,
        .message = "Unauthorized",
        .httpStatus = 401,
    });
    rawTransport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = "ok",
    });
    javelin::jmap::api::RefreshingTransport transport{rawTransport};
    int refreshCalls = 0;
    transport.setRefreshHandler(
        [&](std::string accountId,
            std::string rejectedAccessToken) -> QCoro::Task<std::optional<std::string>>
        {
            ++refreshCalls;
            CHECK(accountId == "u1");
            CHECK(rejectedAccessToken == "access-token");
            co_return std::string{"refreshed-token"};
        });
    int dispatchCalls = 0;

    const auto result = QCoro::waitFor(transport.send({
        .method = javelin::jmap::api::HttpMethod::Get,
        .url = QUrl{QStringLiteral("https://mail.example.com/resource")},
        .headers = {{.name = "Authorization", .value = "Bearer access-token"}},
        .body = {},
        .authentication =
            javelin::jmap::api::BearerAuthentication{
                .accountId = "u1",
                .accessToken = "access-token",
            },
        .cancellation = {},
        .dispatched = [&dispatchCalls] { ++dispatchCalls; },
    }));

    REQUIRE(std::holds_alternative<javelin::jmap::api::HttpResponse>(result));
    CHECK(refreshCalls == 1);
    CHECK(dispatchCalls == 1);
    REQUIRE(rawTransport.requests.size() == 2);
    REQUIRE(rawTransport.requests[1].authentication.has_value());
    CHECK(rawTransport.requests[1].authentication->accessToken == "refreshed-token");
    const auto* refreshedAuthorization =
        header(rawTransport.requests[1], QByteArrayLiteral("Authorization"));
    REQUIRE(refreshedAuthorization != nullptr);
    CHECK(refreshedAuthorization->value == "Bearer refreshed-token");
}

TEST_CASE("refreshing HTTP file transport retries unauthorized downloads",
          "[jmap][transport][auth][file]")
{
    ensureApplication();

    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("download.eml"));
    FakeTransport rawTransport;
    rawTransport.queuedResults.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::HttpFailure,
        .message = "Unauthorized",
        .httpStatus = 401,
    });
    rawTransport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = "streamed body",
    });
    javelin::jmap::api::RefreshingTransport transport{rawTransport};
    int refreshCalls = 0;
    transport.setRefreshHandler(
        [&](std::string, std::string) -> QCoro::Task<std::optional<std::string>>
        {
            ++refreshCalls;
            co_return std::string{"refreshed-token"};
        });

    const auto result = QCoro::waitFor(transport.sendToFile(
        {
            .method = javelin::jmap::api::HttpMethod::Get,
            .url = QUrl{QStringLiteral("https://mail.example.com/resource")},
            .headers = {{.name = "Authorization", .value = "Bearer access-token"}},
            .body = {},
            .authentication =
                javelin::jmap::api::BearerAuthentication{
                    .accountId = "u1",
                    .accessToken = "access-token",
                },
            .cancellation = {},
            .dispatched = {},
        },
        path));

    REQUIRE(std::holds_alternative<javelin::jmap::api::HttpFileResponse>(result));
    CHECK(std::get<javelin::jmap::api::HttpFileResponse>(result).size == 13);
    CHECK(refreshCalls == 1);
    REQUIRE(rawTransport.requests.size() == 2);
    CHECK(rawTransport.requests.back().authentication->accessToken == "refreshed-token");
    QFile file{path};
    REQUIRE(file.open(QIODevice::ReadOnly));
    CHECK(file.readAll() == QByteArrayLiteral("streamed body"));
}

TEST_CASE("blob upload streams a file to the expanded JMAP upload URL",
          "[jmap][transport][upload][file]")
{
    ensureApplication();

    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("message.eml"));
    QFile source{path};
    REQUIRE(source.open(QIODevice::WriteOnly));
    const QByteArray payload = QByteArrayLiteral("From: sender@example.com\r\n\r\nraw message");
    REQUIRE(source.write(payload) == payload.size());
    source.close();

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 201,
        .body =
            R"({"accountId":"destination/account","blobId":"blob-1","type":"message/rfc822","size":40})",
    });

    javelin::jmap::api::Session session;
    session.uploadUrl = "https://mail.example.com/upload/{accountId}";
    session.capabilities.core = true;
    session.capabilities.coreDetails.emplace();
    session.capabilities.coreDetails->maxSizeUpload = 1024;
    javelin::jmap::api::Account destination;
    destination.id = "destination/account";
    session.accounts.emplace(destination.id, destination);

    const auto contextResult =
        javelin::jmap::api::blobUploadContext(session, "destination/account");
    REQUIRE(std::holds_alternative<javelin::jmap::api::BlobUploadContext>(contextResult));
    const auto result = QCoro::waitFor(javelin::jmap::api::uploadBlobFromFile(
        transport, std::get<javelin::jmap::api::BlobUploadContext>(contextResult), "auth-account",
        "destination/account", "access-token", path, "message/rfc822"));

    REQUIRE(std::holds_alternative<javelin::jmap::api::BlobUploadResponse>(result));
    const auto& response = std::get<javelin::jmap::api::BlobUploadResponse>(result);
    CHECK(response.accountId == "destination/account");
    CHECK(response.blobId == "blob-1");
    CHECK(response.type == "message/rfc822");
    CHECK(response.size == 40);
    REQUIRE(transport.requests.size() == 1);
    CHECK(transport.requests.front().url.toEncoded().contains("destination%2Faccount"));
    CHECK(transport.requests.front().body == payload);
    REQUIRE(transport.requests.front().authentication.has_value());
    CHECK(transport.requests.front().authentication->accountId == "auth-account");
    const auto* contentType = header(transport.requests.front(), QByteArrayLiteral("Content-Type"));
    REQUIRE(contentType != nullptr);
    CHECK(contentType->value == "message/rfc822");
}

TEST_CASE("blob upload rejects a file above the advertised JMAP upload limit before dispatch",
          "[jmap][transport][upload][file]")
{
    ensureApplication();

    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("message.eml"));
    QFile source{path};
    REQUIRE(source.open(QIODevice::WriteOnly));
    REQUIRE(source.write(QByteArrayLiteral("too large")) == 9);
    source.close();

    FakeTransport transport;
    javelin::jmap::api::Session session;
    session.uploadUrl = "https://mail.example.com/upload/{accountId}";
    session.capabilities.core = true;
    session.capabilities.coreDetails.emplace();
    session.capabilities.coreDetails->maxSizeUpload = 8;
    javelin::jmap::api::Account destination;
    destination.id = "destination-account";
    session.accounts.emplace(destination.id, destination);

    const auto contextResult =
        javelin::jmap::api::blobUploadContext(session, "destination-account");
    REQUIRE(std::holds_alternative<javelin::jmap::api::BlobUploadContext>(contextResult));
    const auto result = QCoro::waitFor(javelin::jmap::api::uploadBlobFromFile(
        transport, std::get<javelin::jmap::api::BlobUploadContext>(contextResult), "auth-account",
        "destination-account", "access-token", path, "message/rfc822"));

    REQUIRE(std::holds_alternative<javelin::jmap::api::ProtocolError>(result));
    CHECK(std::get<javelin::jmap::api::ProtocolError>(result).code ==
          javelin::jmap::api::ProtocolErrorCode::InvalidRequest);
    CHECK(transport.requests.empty());
}

TEST_CASE("blob upload accepts a file exactly at the advertised JMAP upload limit",
          "[jmap][transport][upload][file]")
{
    ensureApplication();

    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("message.eml"));
    QFile source{path};
    REQUIRE(source.open(QIODevice::WriteOnly));
    REQUIRE(source.write(QByteArrayLiteral("12345678")) == 8);
    source.close();

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 201,
        .body =
            R"({"accountId":"destination-account","blobId":"blob-1","type":"message/rfc822","size":8})",
    });
    javelin::jmap::api::Session session;
    session.uploadUrl = "https://mail.example.com/upload/{accountId}";
    session.capabilities.core = true;
    session.capabilities.coreDetails.emplace();
    session.capabilities.coreDetails->maxSizeUpload = 8;
    javelin::jmap::api::Account destination;
    destination.id = "destination-account";
    session.accounts.emplace(destination.id, destination);

    const auto contextResult =
        javelin::jmap::api::blobUploadContext(session, "destination-account");
    REQUIRE(std::holds_alternative<javelin::jmap::api::BlobUploadContext>(contextResult));
    const auto result = QCoro::waitFor(javelin::jmap::api::uploadBlobFromFile(
        transport, std::get<javelin::jmap::api::BlobUploadContext>(contextResult), "auth-account",
        "destination-account", "access-token", path, "message/rfc822"));

    REQUIRE(std::holds_alternative<javelin::jmap::api::BlobUploadResponse>(result));
    REQUIRE(transport.requests.size() == 1);
    CHECK(transport.requests.front().body == QByteArrayLiteral("12345678"));
}

TEST_CASE("refreshing HTTP file upload retries unauthorized requests from the same source file",
          "[jmap][transport][auth][file]")
{
    ensureApplication();

    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("upload.eml"));
    QFile source{path};
    REQUIRE(source.open(QIODevice::WriteOnly));
    REQUIRE(source.write(QByteArrayLiteral("From: sender@example.com\r\n\r\nstreamed upload")) > 0);
    source.close();

    FakeTransport rawTransport;
    rawTransport.queuedResults.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::HttpFailure,
        .message = "Unauthorized",
        .httpStatus = 401,
    });
    rawTransport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 201,
        .body = "{\"blobId\":\"blob-1\"}",
    });
    javelin::jmap::api::RefreshingTransport transport{rawTransport};
    int refreshCalls = 0;
    transport.setRefreshHandler(
        [&](std::string, std::string) -> QCoro::Task<std::optional<std::string>>
        {
            ++refreshCalls;
            co_return std::string{"refreshed-token"};
        });
    int dispatchCalls = 0;

    const auto result = QCoro::waitFor(transport.sendFromFile(
        {
            .method = javelin::jmap::api::HttpMethod::Post,
            .url = QUrl{QStringLiteral("https://mail.example.com/upload/u1")},
            .headers = {{.name = "Authorization", .value = "Bearer access-token"},
                        {.name = "Content-Type", .value = "message/rfc822"}},
            .body = {},
            .authentication =
                javelin::jmap::api::BearerAuthentication{
                    .accountId = "u1",
                    .accessToken = "access-token",
                },
            .cancellation = {},
            .dispatched = [&dispatchCalls] { ++dispatchCalls; },
        },
        path));

    REQUIRE(std::holds_alternative<javelin::jmap::api::HttpResponse>(result));
    CHECK(std::get<javelin::jmap::api::HttpResponse>(result).statusCode == 201);
    CHECK(refreshCalls == 1);
    CHECK(dispatchCalls == 1);
    REQUIRE(rawTransport.requests.size() == 2);
    CHECK(rawTransport.requests[0].body == rawTransport.requests[1].body);
    CHECK(rawTransport.requests[0].body.contains("streamed upload"));
    REQUIRE(rawTransport.requests[1].authentication.has_value());
    CHECK(rawTransport.requests[1].authentication->accessToken == "refreshed-token");
    const auto* refreshedAuthorization =
        header(rawTransport.requests[1], QByteArrayLiteral("Authorization"));
    REQUIRE(refreshedAuthorization != nullptr);
    CHECK(refreshedAuthorization->value == "Bearer refreshed-token");
}

TEST_CASE("refreshing HTTP device upload rewinds the streamed body after authentication refresh",
          "[jmap][transport][auth][device]")
{
    ensureApplication();

    QByteArray payload =
        QByteArrayLiteral("From: sender@example.com\r\n\r\nstreamed device upload");
    QBuffer source{&payload};
    REQUIRE(source.open(QIODevice::ReadOnly));

    FakeTransport rawTransport;
    rawTransport.queuedResults.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::HttpFailure,
        .message = "Unauthorized",
        .httpStatus = 401,
    });
    rawTransport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 201,
        .body = "{\"blobId\":\"blob-1\"}",
    });
    javelin::jmap::api::RefreshingTransport transport{rawTransport};
    int refreshCalls = 0;
    transport.setRefreshHandler(
        [&](std::string, std::string) -> QCoro::Task<std::optional<std::string>>
        {
            ++refreshCalls;
            co_return std::string{"refreshed-token"};
        });
    int dispatchCalls = 0;

    const auto result = QCoro::waitFor(transport.sendFromDevice(
        {
            .method = javelin::jmap::api::HttpMethod::Post,
            .url = QUrl{QStringLiteral("https://mail.example.com/upload/u1")},
            .headers = {{.name = "Authorization", .value = "Bearer access-token"},
                        {.name = "Content-Type", .value = "message/rfc822"}},
            .body = {},
            .authentication =
                javelin::jmap::api::BearerAuthentication{
                    .accountId = "u1",
                    .accessToken = "access-token",
                },
            .cancellation = {},
            .dispatched = [&dispatchCalls] { ++dispatchCalls; },
        },
        source, static_cast<std::uint64_t>(payload.size())));

    REQUIRE(std::holds_alternative<javelin::jmap::api::HttpResponse>(result));
    CHECK(refreshCalls == 1);
    CHECK(dispatchCalls == 1);
    REQUIRE(rawTransport.requests.size() == 2);
    CHECK(rawTransport.requests[0].body == payload);
    CHECK(rawTransport.requests[1].body == payload);
    REQUIRE(rawTransport.requests[1].authentication.has_value());
    CHECK(rawTransport.requests[1].authentication->accessToken == "refreshed-token");
}

TEST_CASE("refreshing HTTP transport resolves current tokens for stale request scopes",
          "[jmap][transport][auth]")
{
    ensureApplication();

    FakeTransport rawTransport;
    rawTransport.queuedResults.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::HttpFailure,
        .message = "Unauthorized",
        .httpStatus = 401,
    });
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        rawTransport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
            .statusCode = 200,
            .body = "ok",
        });
    }

    javelin::jmap::api::RefreshingTransport transport{rawTransport};
    std::string currentAccessToken = "access-token";
    transport.setAccessTokenProvider(
        [&currentAccessToken](std::string_view) -> std::optional<std::string>
        { return currentAccessToken; });
    int refreshCalls = 0;
    transport.setRefreshHandler(
        [&](std::string, std::string) -> QCoro::Task<std::optional<std::string>>
        {
            ++refreshCalls;
            currentAccessToken = "refreshed-token";
            co_return std::string{currentAccessToken};
        });
    const auto request = []
    {
        return javelin::jmap::api::HttpRequest{
            .method = javelin::jmap::api::HttpMethod::Get,
            .url = QUrl{QStringLiteral("https://mail.example.com/resource")},
            .headers = {{.name = "Authorization", .value = "Bearer access-token"}},
            .body = {},
            .authentication =
                javelin::jmap::api::BearerAuthentication{
                    .accountId = "u1",
                    .accessToken = "access-token",
                },
            .cancellation = {},
            .dispatched = {},
        };
    };

    const auto first = QCoro::waitFor(transport.send(request()));
    const auto second = QCoro::waitFor(transport.send(request()));

    REQUIRE(std::holds_alternative<javelin::jmap::api::HttpResponse>(first));
    REQUIRE(std::holds_alternative<javelin::jmap::api::HttpResponse>(second));
    CHECK(refreshCalls == 1);
    REQUIRE(rawTransport.requests.size() == 3);
    CHECK(rawTransport.requests[0].authentication->accessToken == "access-token");
    CHECK(rawTransport.requests[1].authentication->accessToken == "refreshed-token");
    CHECK(rawTransport.requests[2].authentication->accessToken == "refreshed-token");
    const auto* refreshedAuthorization =
        header(rawTransport.requests[2], QByteArrayLiteral("Authorization"));
    REQUIRE(refreshedAuthorization != nullptr);
    CHECK(refreshedAuthorization->value == "Bearer refreshed-token");
}

TEST_CASE("refreshing HTTP transport surfaces a second unauthorized response",
          "[jmap][transport][auth]")
{
    ensureApplication();

    FakeTransport rawTransport;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        rawTransport.queuedResults.push_back(javelin::jmap::api::TransportError{
            .code = javelin::jmap::api::TransportErrorCode::HttpFailure,
            .message = "Unauthorized",
            .httpStatus = 401,
        });
    }
    javelin::jmap::api::RefreshingTransport transport{rawTransport};
    int refreshCalls = 0;
    transport.setRefreshHandler(
        [&](std::string, std::string) -> QCoro::Task<std::optional<std::string>>
        {
            ++refreshCalls;
            co_return std::string{"refreshed-token"};
        });

    const auto result = QCoro::waitFor(transport.send({
        .method = javelin::jmap::api::HttpMethod::Get,
        .url = QUrl{QStringLiteral("https://mail.example.com/resource")},
        .headers = {{.name = "Authorization", .value = "Bearer access-token"}},
        .body = {},
        .authentication =
            javelin::jmap::api::BearerAuthentication{
                .accountId = "u1",
                .accessToken = "access-token",
            },
        .cancellation = {},
        .dispatched = {},
    }));

    REQUIRE(std::holds_alternative<javelin::jmap::api::TransportError>(result));
    CHECK(std::get<javelin::jmap::api::TransportError>(result).httpStatus == 401);
    CHECK(refreshCalls == 1);
    CHECK(rawTransport.requests.size() == 2);
}

TEST_CASE("refreshing JMAP transport retries one definite unauthorized response",
          "[jmap][method][transport][auth]")
{
    ensureApplication();
    const auto parsed = javelin::jmap::api::parseResponseEnvelope(
        javelin::tests::loadFixture("jmap/method/response.json"));
    REQUIRE(parsed.ok());

    ScriptedJmapMethodTransport rawTransport;
    rawTransport.results.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::HttpFailure,
        .message = "Unauthorized",
        .httpStatus = 401,
    });
    rawTransport.results.push_back(*parsed.value);
    javelin::jmap::api::RefreshingJmapMethodTransport transport{rawTransport};
    int refreshCalls = 0;
    transport.setRefreshHandler(
        [&](std::string accountId,
            std::string rejectedAccessToken) -> QCoro::Task<std::optional<std::string>>
        {
            ++refreshCalls;
            CHECK(accountId == "u1");
            CHECK(rejectedAccessToken == "access-token");
            co_return std::string{"refreshed-token"};
        });
    javelin::jmap::api::MethodCaller caller{transport};

    const auto result = QCoro::waitFor(caller.call(makeRequestContext(), loadRequestEnvelope()));

    REQUIRE(std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(result));
    CHECK(refreshCalls == 1);
    REQUIRE(rawTransport.requests.size() == 2);
    CHECK(rawTransport.requests.front().accessToken == "access-token");
    CHECK(rawTransport.requests.back().accessToken == "refreshed-token");
}

TEST_CASE("refreshing JMAP transport resolves current tokens for stale request scopes",
          "[jmap][method][transport][auth]")
{
    ensureApplication();
    const auto parsed = javelin::jmap::api::parseResponseEnvelope(
        javelin::tests::loadFixture("jmap/method/response.json"));
    REQUIRE(parsed.ok());

    ScriptedJmapMethodTransport rawTransport;
    rawTransport.results.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::HttpFailure,
        .message = "Unauthorized",
        .httpStatus = 401,
    });
    rawTransport.results.push_back(*parsed.value);
    rawTransport.results.push_back(*parsed.value);

    javelin::jmap::api::RefreshingJmapMethodTransport transport{rawTransport};
    std::string currentAccessToken = "access-token";
    transport.setAccessTokenProvider(
        [&currentAccessToken](std::string_view) -> std::optional<std::string>
        { return currentAccessToken; });
    int refreshCalls = 0;
    transport.setRefreshHandler(
        [&](std::string, std::string) -> QCoro::Task<std::optional<std::string>>
        {
            ++refreshCalls;
            currentAccessToken = "refreshed-token";
            co_return std::string{currentAccessToken};
        });
    javelin::jmap::api::MethodCaller caller{transport};

    const auto first = QCoro::waitFor(caller.call(makeRequestContext(), loadRequestEnvelope()));
    const auto second = QCoro::waitFor(caller.call(makeRequestContext(), loadRequestEnvelope()));

    REQUIRE(std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(first));
    REQUIRE(std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(second));
    CHECK(refreshCalls == 1);
    REQUIRE(rawTransport.requests.size() == 3);
    CHECK(rawTransport.requests[0].accessToken == "access-token");
    CHECK(rawTransport.requests[1].accessToken == "refreshed-token");
    CHECK(rawTransport.requests[2].accessToken == "refreshed-token");
}

TEST_CASE("refreshing JMAP transport surfaces a second unauthorized response",
          "[jmap][method][transport][auth]")
{
    ensureApplication();

    ScriptedJmapMethodTransport rawTransport;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        rawTransport.results.push_back(javelin::jmap::api::TransportError{
            .code = javelin::jmap::api::TransportErrorCode::HttpFailure,
            .message = "Unauthorized",
            .httpStatus = 401,
        });
    }
    javelin::jmap::api::RefreshingJmapMethodTransport transport{rawTransport};
    int refreshCalls = 0;
    transport.setRefreshHandler(
        [&](std::string, std::string) -> QCoro::Task<std::optional<std::string>>
        {
            ++refreshCalls;
            co_return std::string{"refreshed-token"};
        });
    javelin::jmap::api::MethodCaller caller{transport};

    const auto result = QCoro::waitFor(caller.call(makeRequestContext(), loadRequestEnvelope()));

    REQUIRE(std::holds_alternative<javelin::jmap::api::TransportError>(result));
    CHECK(std::get<javelin::jmap::api::TransportError>(result).httpStatus == 401);
    CHECK(refreshCalls == 1);
    CHECK(rawTransport.requests.size() == 2);
}

TEST_CASE("method caller propagates transport failures", "[jmap][method][transport]")
{
    ensureApplication();

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::HttpFailure,
        .message = "Unauthorized",
        .httpStatus = 401,
    });

    javelin::jmap::api::MethodCaller methodCaller{transport.methodTransport};
    const auto result =
        QCoro::waitFor(methodCaller.call(makeRequestContext(), loadRequestEnvelope()));

    REQUIRE(std::holds_alternative<javelin::jmap::api::TransportError>(result));
    CHECK(std::get<javelin::jmap::api::TransportError>(result).httpStatus == 401);
}

TEST_CASE("method caller maps malformed responses to protocol errors", "[jmap][method][transport]")
{
    ensureApplication();

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = "{not json}",
    });

    javelin::jmap::api::MethodCaller methodCaller{transport.methodTransport};
    const auto result =
        QCoro::waitFor(methodCaller.call(makeRequestContext(), loadRequestEnvelope()));

    REQUIRE(std::holds_alternative<javelin::jmap::api::ProtocolError>(result));
    CHECK(std::get<javelin::jmap::api::ProtocolError>(result).code ==
          javelin::jmap::api::ProtocolErrorCode::InvalidResponse);
}
