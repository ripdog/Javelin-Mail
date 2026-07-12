#include "jmap/JmapCore.h"

#include "FixtureReader.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/SessionRepository.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <memory>
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

    class FakeTransport final : public javelin::jmap::api::AbstractTransport
    {
      public:
        std::vector<javelin::jmap::api::TransportResult> queuedResults;
        std::vector<javelin::jmap::api::HttpRequest> requests;

        [[nodiscard]] QCoro::Task<javelin::jmap::api::TransportResult>
        send(javelin::jmap::api::HttpRequest request) override
        {
            requests.push_back(std::move(request));
            REQUIRE_FALSE(queuedResults.empty());
            auto result = std::move(queuedResults.front());
            queuedResults.erase(queuedResults.begin());
            co_return result;
        }
    };

    [[nodiscard]] QString makeConnectionName()
    {
        static int counter = 0;
        ++counter;
        return QStringLiteral("javelin-core-session-refresh-%1").arg(counter);
    }
} // namespace

TEST_CASE("JmapCore exposes a non-empty status summary", "[jmap]")
{
    const javelin::jmap::JmapCore core;

    CHECK_FALSE(core.statusSummary().isEmpty());
    CHECK(core.statusSummary().contains(QStringLiteral("JMAP")));
}

TEST_CASE("JmapCore startup session refresh discovers and caches websocket capability",
          "[jmap][core][session][websocket]")
{
    ensureApplication();
    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());

    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = temporaryDir.filePath(QStringLiteral("cache.sqlite3")),
    });
    if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
    {
        FAIL(error->message.toStdString());
    }
    auto database =
        std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArray::fromStdString(
            javelin::tests::loadFixture("jmap/session/websocket_session.json")),
    });
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::JmapCore core{database, transport, methodTransport};

    const auto result = QCoro::waitFor(core.refreshSession(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1"));

    REQUIRE(std::holds_alternative<javelin::jmap::SessionRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::SessionRefreshSummary>(result);
    CHECK(summary.ownerAccountId == "u1");
    CHECK(summary.websocketAdvertised);
    CHECK(summary.websocketPushSupported);
    CHECK(summary.resolvedSessionUrl == "https://mail.example.com/.well-known/jmap");
    REQUIRE(transport.requests.size() == 1);
    CHECK(transport.requests.front().url ==
          QUrl{QStringLiteral("https://mail.example.com/.well-known/jmap")});

    javelin::jmap::cache::SessionRepository sessions{database};
    const auto loaded = sessions.load("u1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::api::Session>>(loaded));
    REQUIRE(std::get<std::optional<javelin::jmap::api::Session>>(loaded).has_value());
    const auto& session =
        *std::get<std::optional<javelin::jmap::api::Session>>(loaded);
    REQUIRE(session.capabilities.websocket.has_value());
    CHECK(session.capabilities.websocket->url == "wss://mail.example.com/jmap/ws");
    CHECK(session.capabilities.websocket->supportsPush);
}
