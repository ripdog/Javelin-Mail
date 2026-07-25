#include "gui/shell/TabPersistence.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

TEST_CASE("mailbox tab persistence preserves identity position and selection",
          "[gui][tabs][persistence]")
{
    const auto persisted = javelin::gui::shell::persistMailboxTab({
        .accountId = "account-a",
        .title = QStringLiteral("Archive"),
        .selection =
            {
                .threadId = "thread-a",
                .emailId = "email-a",
                .selectedEmailIds = {"email-a", "email-b"},
            },
        .mailboxId = "archive",
        .mailboxRole = "archive",
        .offset = 300,
    });

    CHECK(persisted.common.accountId == "account-a");
    CHECK(persisted.common.title == QStringLiteral("Archive"));
    CHECK(persisted.common.selection.threadId == std::optional<std::string>{"thread-a"});
    CHECK(persisted.common.selection.emailId == std::optional<std::string>{"email-a"});
    CHECK(persisted.mailboxId == "archive");
    CHECK(persisted.mailboxRole == std::optional<std::string>{"archive"});
    CHECK(persisted.offset == 300);
}

TEST_CASE("search tab persistence preserves resumable search state", "[gui][tabs][persistence]")
{
    const auto persisted = javelin::gui::shell::persistSearchTab({
        .accountId = "account-a",
        .title = QStringLiteral("Search: Ada"),
        .selection = {.threadId = "thread-a", .emailId = "email-a", .selectedEmailIds = {}},
        .criteria = {.text = "Ada", .from = "sender@example.test"},
        .page =
            {
                .offset = 40,
                .position = 40,
                .returnedLimit = 20,
                .total = 63,
                .queryState = "query-state",
                .anchor = std::nullopt,
                .items = {},
                .cacheLoaded = true,
                .refreshInFlight = false,
                .stale = true,
                .refreshError = {},
            },
        .mode = javelin::app::SearchMode::Online,
        .sessionId = "session-a",
    });

    CHECK(persisted.search.criteria.text == std::optional<std::string>{"Ada"});
    CHECK(persisted.search.criteria.from == std::optional<std::string>{"sender@example.test"});
    CHECK(persisted.search.restored.page.offset == 40);
    CHECK(persisted.search.restored.page.total == std::optional<std::size_t>{63});
    CHECK(persisted.search.restored.page.stale);
    CHECK(persisted.search.restored.mode == javelin::app::SearchMode::Online);
    CHECK(persisted.search.restored.sessionId == "session-a");
}

TEST_CASE("mailbox tab restoration creates a cache-only initial page", "[gui][tabs][persistence]")
{
    const auto plan = javelin::gui::shell::planMailboxTabRestore(
        {
            .common =
                {
                    .accountId = "account-a",
                    .title = {},
                    .selection = {.threadId = "thread-a", .emailId = "email-a"},
                },
            .mailboxId = "archive",
            .mailboxRole = "archive",
            .offset = 200,
        },
        100);

    CHECK(plan.accountId == "account-a");
    CHECK(plan.mailboxId == "archive");
    CHECK(plan.title == QStringLiteral("archive"));
    CHECK(plan.restored.page.offset == 200);
    CHECK(plan.restored.page.position == 200);
    CHECK(plan.restored.page.returnedLimit == 100);
    CHECK_FALSE(plan.restored.page.cacheLoaded);
    CHECK_FALSE(plan.restored.page.stale);
    CHECK(plan.selection.threadId == std::optional<std::string>{"thread-a"});
    CHECK(plan.selection.selectedEmailIds.empty());
}

TEST_CASE("search tab restoration transfers persisted session state", "[gui][tabs][persistence]")
{
    auto persisted = javelin::gui::shell::PersistedSearchTab{
        .common =
            {
                .accountId = "account-a",
                .title = QStringLiteral("Search"),
                .selection = {.threadId = "thread-a", .emailId = "email-a"},
            },
        .search =
            {
                .criteria = {.subject = "report"},
                .restored =
                    {
                        .page =
                            {
                                .offset = 20,
                                .position = 20,
                                .returnedLimit = 20,
                                .total = std::nullopt,
                                .queryState = {},
                                .anchor = std::nullopt,
                                .items = {},
                                .cacheLoaded = false,
                                .refreshInFlight = false,
                                .stale = false,
                                .refreshError = {},
                            },
                        .mode = javelin::app::SearchMode::Promoting,
                        .sessionId = "session-a",
                    },
            },
    };

    const auto plan = javelin::gui::shell::planSearchTabRestore(std::move(persisted));

    CHECK(plan.accountId == "account-a");
    CHECK(plan.criteria.subject == std::optional<std::string>{"report"});
    CHECK(plan.restored.page.offset == 20);
    CHECK(plan.restored.mode == javelin::app::SearchMode::Promoting);
    CHECK(plan.restored.sessionId == "session-a");
    CHECK(plan.selection.emailId == std::optional<std::string>{"email-a"});
    CHECK(plan.selection.selectedEmailIds.empty());
}
