#include "jmap/api/Transport.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimeZone>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <variant>

namespace
{
    void ensureApplication()
    {
        if (QCoreApplication::instance() != nullptr)
            return;
        static int argc = 1;
        static char appName[] = "javelin-tests";
        static char* argv[] = {appName, nullptr};
        static QCoreApplication application(argc, argv);
        Q_UNUSED(application);
    }

    [[nodiscard]] bool waitUntil(const std::function<bool()>& predicate)
    {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        return predicate();
    }

    [[nodiscard]] QDateTime referenceTime()
    {
        return QDateTime{QDate{1994, 11, 6}, QTime{8, 49, 7}, QTimeZone::UTC};
    }
} // namespace

TEST_CASE("Retry-After accepts delay-seconds and HTTP-date values", "[jmap][transport]")
{
    using javelin::jmap::api::parseRetryAfter;
    using std::chrono::seconds;

    CHECK(parseRetryAfter("120", referenceTime()) == seconds{120});
    CHECK(parseRetryAfter(" 5 ", referenceTime()) == seconds{5});
    CHECK(parseRetryAfter("Sun, 06 Nov 1994 08:49:37 GMT", referenceTime()) == seconds{30});
    CHECK(parseRetryAfter("Sun, 06 Nov 1994 08:00:00 GMT", referenceTime()) == seconds{0});
    CHECK_FALSE(parseRetryAfter("-1", referenceTime()).has_value());
    CHECK_FALSE(parseRetryAfter("soon", referenceTime()).has_value());
    CHECK_FALSE(parseRetryAfter("", referenceTime()).has_value());
}

TEST_CASE("invalidating one account's connections leaves other accounts' requests running",
          "[jmap][transport]")
{
    ensureApplication();
    QTcpServer server;
    REQUIRE(server.listen(QHostAddress::LocalHost));
    std::vector<QTcpSocket*> connections;
    QObject::connect(&server, &QTcpServer::newConnection, &server,
                     [&server, &connections]
                     {
                         while (auto* socket = server.nextPendingConnection())
                             connections.push_back(socket);
                     });

    QNetworkAccessManager networkAccessManager;
    javelin::jmap::api::QtNetworkTransport transport{networkAccessManager};
    const QUrl url{QStringLiteral("http://127.0.0.1:%1/api").arg(server.serverPort())};

    const auto request =
        [&url](std::string accountId, const javelin::jmap::api::CancellationToken& cancellation)
    {
        return javelin::jmap::api::HttpRequest{
            .method = javelin::jmap::api::HttpMethod::Post,
            .url = url,
            .headers = {},
            .body = QByteArrayLiteral("{}"),
            .authentication =
                javelin::jmap::api::BearerAuthentication{
                    .accountId = std::move(accountId),
                    .accessToken = "token",
                },
            .cancellation = cancellation,
            .dispatched = {},
        };
    };

    javelin::jmap::api::CancellationSource otherCancellation;
    std::optional<javelin::jmap::api::TransportResult> invalidatedResult;
    std::optional<javelin::jmap::api::TransportResult> otherResult;
    QObject context;
    QCoro::connect(transport.send(request("account-a", {})), &context,
                   [&invalidatedResult](javelin::jmap::api::TransportResult result)
                   { invalidatedResult = std::move(result); });
    QCoro::connect(transport.send(request("account-b", otherCancellation.token())), &context,
                   [&otherResult](javelin::jmap::api::TransportResult result)
                   { otherResult = std::move(result); });
    REQUIRE(waitUntil([&connections] { return connections.size() == 2; }));

    transport.invalidateConnections("account-a");

    REQUIRE(waitUntil([&invalidatedResult] { return invalidatedResult.has_value(); }));
    const auto* invalidatedError =
        std::get_if<javelin::jmap::api::TransportError>(&*invalidatedResult);
    REQUIRE(invalidatedError != nullptr);
    CHECK(invalidatedError->code == javelin::jmap::api::TransportErrorCode::NetworkFailure);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    CHECK_FALSE(otherResult.has_value());

    otherCancellation.cancel();
    REQUIRE(waitUntil([&otherResult] { return otherResult.has_value(); }));
    const auto* otherError = std::get_if<javelin::jmap::api::TransportError>(&*otherResult);
    REQUIRE(otherError != nullptr);
    CHECK(otherError->code == javelin::jmap::api::TransportErrorCode::Cancelled);
}
