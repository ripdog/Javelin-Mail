#include "gui/messages/InfiniteScroll.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("infinite scrolling loads ahead before the viewport reaches the end",
          "[gui][messages][infinite-scroll]")
{
    CHECK(javelin::gui::messages::shouldLoadMoreMessages(780, 1000, 100, 100));
    CHECK_FALSE(javelin::gui::messages::shouldLoadMoreMessages(700, 1000, 100, 100));
}

TEST_CASE("infinite scrolling fills a viewport that has no scrollbar yet",
          "[gui][messages][infinite-scroll]")
{
    CHECK(javelin::gui::messages::shouldLoadMoreMessages(0, 0, 500, 20));
    CHECK_FALSE(javelin::gui::messages::shouldLoadMoreMessages(0, 0, 500, 0));
}
