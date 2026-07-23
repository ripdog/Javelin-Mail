#include "gui/messages/Pagination.h"
#include "app/MessageListSession.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("pagination metrics count conversations rather than expanded model rows",
          "[gui][pagination]")
{
    const auto metrics = javelin::gui::messages::pageMetrics(100, 100, 1000);
    CHECK(metrics.start == 101);
    CHECK(metrics.end == 200);
    CHECK(metrics.hasNext);

    const std::size_t expandedVisibleRows = 107;
    CHECK(metrics.end != 100 + expandedVisibleRows);
}

TEST_CASE("pagination metrics clamp short final pages and empty totals", "[gui][pagination]")
{
    const auto finalPage = javelin::gui::messages::pageMetrics(200, 37, 237);
    CHECK(finalPage.start == 201);
    CHECK(finalPage.end == 237);
    CHECK_FALSE(finalPage.hasNext);

    const auto empty = javelin::gui::messages::pageMetrics(0, 0, 0);
    CHECK(empty.start == 0);
    CHECK(empty.end == 0);
    CHECK_FALSE(empty.hasNext);
}

TEST_CASE("pagination relocates offsets after a total shrinks", "[gui][pagination]")
{
    CHECK(javelin::gui::messages::normalizedPageOffset(300, 237, 50) == 200);
    CHECK(javelin::gui::messages::normalizedPageOffset(100, 237, 50) == 100);
    CHECK(javelin::gui::messages::normalizedPageOffset(100, 0, 50) == 0);
}

TEST_CASE("pagination converts between arbitrary pages and query offsets", "[gui][pagination]")
{
    CHECK(javelin::gui::messages::pageCount(0, 100) == 0);
    CHECK(javelin::gui::messages::pageCount(237, 100) == 3);
    CHECK(javelin::gui::messages::pageCount(200, 100) == 2);
    CHECK(javelin::gui::messages::pageIndex(200, 100) == 2);
    CHECK(javelin::gui::messages::pageOffset(2, 100) == 200);
    CHECK(javelin::app::messageListPageCount(237, 100) == 3);
    CHECK(javelin::app::messageListPageOffset(2, 100) == 200);
    CHECK(javelin::app::normalizedMessageListPageOffset(300, 237, 50) == 200);
}

TEST_CASE("superseded page refreshes cannot complete a newer navigation", "[gui][pagination]")
{
    javelin::gui::messages::PageRefreshState refresh;
    REQUIRE(refresh.begin(41));
    CHECK(refresh.isInFlight());
    CHECK_FALSE(refresh.begin(42));

    refresh.supersede();
    CHECK_FALSE(refresh.isInFlight());
    REQUIRE(refresh.begin(42));
    CHECK_FALSE(refresh.complete(41));
    CHECK(refresh.isInFlight());
    CHECK(refresh.complete(42));
    CHECK_FALSE(refresh.isInFlight());
}
