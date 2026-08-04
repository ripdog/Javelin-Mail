#include "app/OAuthRefreshSingleFlight.h"

#include <QCoroTask>
#include <QCoroTimer>

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>

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
} // namespace

TEST_CASE("OAuth refresh single flight shares one result with every waiter", "[app][auth][oauth]")
{
    using namespace std::chrono_literals;

    ensureApplication();
    javelin::app::OAuthRefreshSingleFlight refreshes;
    int operationCalls = 0;
    const auto operation = [&]() -> QCoro::Task<javelin::app::OAuthRefreshOutcome>
    {
        ++operationCalls;
        QTimer timer;
        timer.setSingleShot(true);
        timer.start(1ms);
        co_await qCoro(timer).waitForTimeout();
        co_return javelin::app::OAuthRefreshOutcome{
            .succeeded = true,
            .accessToken = QStringLiteral("refreshed-token"),
        };
    };

    std::optional<javelin::app::OAuthRefreshOutcome> first;
    std::optional<javelin::app::OAuthRefreshOutcome> second;
    int completed = 0;
    QEventLoop loop;
    const auto complete = [&](std::optional<javelin::app::OAuthRefreshOutcome>& destination,
                              javelin::app::OAuthRefreshOutcome outcome)
    {
        destination = std::move(outcome);
        if (++completed == 2)
            loop.quit();
    };

    auto firstTask = refreshes.run(QStringLiteral("connection"), operation);
    QCoro::connect(std::move(firstTask), &refreshes, [&](javelin::app::OAuthRefreshOutcome outcome)
                   { complete(first, std::move(outcome)); });
    auto secondTask = refreshes.run(QStringLiteral("connection"), operation);
    QCoro::connect(std::move(secondTask), &refreshes, [&](javelin::app::OAuthRefreshOutcome outcome)
                   { complete(second, std::move(outcome)); });
    loop.exec();

    CHECK(operationCalls == 1);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->succeeded);
    CHECK(second->succeeded);
    CHECK(first->accessToken == QStringLiteral("refreshed-token"));
    CHECK(second->accessToken == QStringLiteral("refreshed-token"));
}

TEST_CASE("OAuth refresh single flight isolates different connections", "[app][auth][oauth]")
{
    ensureApplication();
    javelin::app::OAuthRefreshSingleFlight refreshes;
    int operationCalls = 0;
    const auto operation = [&]() -> QCoro::Task<javelin::app::OAuthRefreshOutcome>
    {
        ++operationCalls;
        co_return javelin::app::OAuthRefreshOutcome{
            .succeeded = true,
            .accessToken = QStringLiteral("token"),
        };
    };

    const auto first = QCoro::waitFor(refreshes.run(QStringLiteral("connection-a"), operation));
    const auto second = QCoro::waitFor(refreshes.run(QStringLiteral("connection-b"), operation));

    CHECK(operationCalls == 2);
    CHECK(first.succeeded);
    CHECK(second.succeeded);
}
