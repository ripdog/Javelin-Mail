#include "gui/messages/MessageSelectionRestoration.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

namespace
{
    const std::vector<javelin::gui::messages::MessageRowIdentity> rows{
        {.threadId = "thread-a", .emailId = "email-a"},
        {.threadId = "thread-b", .emailId = "email-b"},
        {.threadId = "thread-c", .emailId = "email-c"},
    };
}

TEST_CASE("message selection restoration preserves a matching multi-selection",
          "[gui][messages][selection]")
{
    const auto plan = javelin::gui::messages::planMessageSelectionRestoration(
        rows, {
                  .threadId = "thread-b",
                  .emailId = "email-b",
                  .selectedEmailIds = {"email-a", "email-b"},
                  .previousRow = 2,
              });

    CHECK(plan.selectedRows == std::vector<std::size_t>{0, 1});
    CHECK(plan.currentRow == std::optional<std::size_t>{1});
    CHECK_FALSE(plan.currentEmailChanged);
    CHECK_FALSE(plan.fallbackSelected);
}

TEST_CASE("message selection restoration chooses the final surviving selected email",
          "[gui][messages][selection]")
{
    const auto plan = javelin::gui::messages::planMessageSelectionRestoration(
        rows, {
                  .threadId = std::nullopt,
                  .emailId = "email-missing",
                  .selectedEmailIds = {"email-a", "email-c"},
                  .previousRow = std::nullopt,
              });

    CHECK(plan.selectedRows == std::vector<std::size_t>{0, 2});
    CHECK(plan.currentRow == std::optional<std::size_t>{2});
}

TEST_CASE("message selection restoration falls back from a missing email to its thread",
          "[gui][messages][selection]")
{
    const auto plan = javelin::gui::messages::planMessageSelectionRestoration(
        rows, {
                  .threadId = "thread-b",
                  .emailId = "email-missing",
                  .selectedEmailIds = {},
                  .previousRow = std::nullopt,
              });

    CHECK(plan.selectedRows.empty());
    CHECK(plan.currentRow == std::optional<std::size_t>{1});
    CHECK(plan.currentEmailChanged);
    CHECK_FALSE(plan.fallbackSelected);
}

TEST_CASE("message selection restoration keeps the nearest row after removal",
          "[gui][messages][selection]")
{
    const auto plan = javelin::gui::messages::planMessageSelectionRestoration(
        rows, {
                  .threadId = std::nullopt,
                  .emailId = "email-missing",
                  .selectedEmailIds = {},
                  .previousRow = 8,
              });

    CHECK(plan.currentRow == std::optional<std::size_t>{2});
    CHECK(plan.fallbackSelected);
}

TEST_CASE("message selection restoration leaves an empty page unselected",
          "[gui][messages][selection]")
{
    const auto plan =
        javelin::gui::messages::planMessageSelectionRestoration({}, {
                                                                        .threadId = "thread-a",
                                                                        .emailId = "email-a",
                                                                        .selectedEmailIds = {},
                                                                        .previousRow = 0,
                                                                    });

    CHECK(plan.selectedRows.empty());
    CHECK_FALSE(plan.currentRow.has_value());
    CHECK_FALSE(plan.fallbackSelected);
}
