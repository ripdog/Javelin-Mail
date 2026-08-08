#include "gui/shell/TabPersistence.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

TEST_CASE("mailbox tab persistence preserves identity, selection, and loaded window manifest",
          "[gui][tabs][persistence][infinite-scroll]")
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
        .windows = {{.offset = 0, .limit = 100}, {.offset = 100, .limit = 100}},
    });

    CHECK(persisted.accountId == "account-a");
    CHECK(persisted.common.title == QStringLiteral("Archive"));
    CHECK(persisted.common.selection.threadId == std::optional<std::string>{"thread-a"});
    CHECK(persisted.common.selection.emailId == std::optional<std::string>{"email-a"});
    CHECK(persisted.mailboxId == "archive");
    CHECK(persisted.mailboxRole == std::optional<std::string>{"archive"});
    CHECK(persisted.windows.size() == 2);
}

TEST_CASE("search tab persistence preserves query identity and window manifest without result rows",
          "[gui][tabs][persistence][infinite-scroll]")
{
    const auto persisted = javelin::gui::shell::persistSearchTab({
        .accountId = "account-a",
        .title = QStringLiteral("Search: Ada"),
        .selection = {.threadId = "thread-a", .emailId = "email-a", .selectedEmailIds = {}},
        .criteria = {.text = "Ada", .from = "sender@example.test"},
        .mode = javelin::app::SearchMode::Online,
        .sessionId = "session-a",
        .windows = {{.offset = 0, .limit = 100}},
    });

    CHECK(persisted.search.criteria.text == std::optional<std::string>{"Ada"});
    CHECK(persisted.search.criteria.from == std::optional<std::string>{"sender@example.test"});
    CHECK(persisted.search.restored.mode == javelin::app::SearchMode::Online);
    CHECK(persisted.search.restored.sessionId == "session-a");
    CHECK(persisted.search.restored.windows.size() == 1);
}

TEST_CASE("mailbox tab restoration preserves its compact infinite-scroll window manifest",
          "[gui][tabs][persistence][infinite-scroll]")
{
    const auto plan = javelin::gui::shell::planMailboxTabRestore({
        .common =
            {
                .title = {},
                .selection = {.threadId = "thread-a", .emailId = "email-a"},
            },
        .accountId = "account-a",
        .mailboxId = "archive",
        .mailboxRole = "archive",
        .windows = {{.offset = 0, .limit = 100}, {.offset = 100, .limit = 100}},
    });

    CHECK(plan.accountId == "account-a");
    CHECK(plan.mailboxId == "archive");
    CHECK(plan.title == QStringLiteral("archive"));
    CHECK(plan.restored.windows.size() == 2);
    CHECK(plan.selection.threadId == std::optional<std::string>{"thread-a"});
    CHECK(plan.selection.selectedEmailIds.empty());
}

TEST_CASE("workspace tab runtime states survive persistence round trip", "[gui][tabs][persistence]")
{
    using namespace javelin::gui::shell;

    const TabState contactsTab{
        .content = ContactsTabState{.title = QStringLiteral("Contacts"),
                                    .widget = nullptr,
                                    .selection = {}},
    };
    const TabState calendarTab{
        .content = CalendarTabState{.title = QStringLiteral("Calendar"),
                                    .widget = nullptr,
                                    .selection = {}},
    };

    PersistedMainWindowState state;
    state.tabs.push_back(persistContactsTab(std::get<ContactsTabState>(contactsTab.content),
                                            {
                                                .accountId = "contacts-account",
                                                .addressBookId = "address-book",
                                                .contactId = "contact",
                                                .filter = QStringLiteral("Ada"),
                                                .sortMode = 0,
                                                .groupFilterMode = 0,
                                                .groupId = {},
                                                .selectedContactKeys = {},
                                            }));
    state.tabs.push_back(
        persistCalendarTab(std::get<CalendarTabState>(calendarTab.content), QDate{2026, 8, 1}));

    const auto restored = deserializeMainWindowState(serializeMainWindowState(state), {});

    REQUIRE(restored.tabs.size() == 2);
    const auto& contacts = std::get<PersistedContactsTab>(restored.tabs[0]);
    CHECK(contacts.common.title == QStringLiteral("Contacts"));
    CHECK(contacts.view.accountId == "contacts-account");
    CHECK(contacts.view.addressBookId == "address-book");
    CHECK(contacts.view.contactId == "contact");
    CHECK(contacts.view.filter == QStringLiteral("Ada"));
    const auto& calendar = std::get<PersistedCalendarTab>(restored.tabs[1]);
    CHECK(calendar.common.title == QStringLiteral("Calendar"));
    CHECK((calendar.displayedMonth == QDate{2026, 8, 1}));
}

TEST_CASE("active tab restoration follows the persisted tab identity", "[gui][tabs][persistence]")
{
    using javelin::gui::shell::resolveRestoredActiveTabIndex;

    CHECK(resolveRestoredActiveTabIndex(2, {0, std::nullopt, 1, 2}) == std::optional<int>{1});
    CHECK(resolveRestoredActiveTabIndex(1, {0, std::nullopt, 1, 2}) == std::optional<int>{0});
    CHECK(resolveRestoredActiveTabIndex(0, {std::nullopt, 0, 1}) == std::optional<int>{0});
    CHECK_FALSE(resolveRestoredActiveTabIndex(0, {}).has_value());
}

TEST_CASE("search tab restoration transfers persisted session identity and window manifest",
          "[gui][tabs][persistence]")
{
    auto persisted = javelin::gui::shell::PersistedSearchTab{
        .common =
            {
                .title = QStringLiteral("Search"),
                .selection = {.threadId = "thread-a", .emailId = "email-a"},
            },
        .accountId = "account-a",
        .search =
            {
                .criteria = {.subject = "report"},
                .restored =
                    {
                        .mode = javelin::app::SearchMode::Promoting,
                        .sessionId = "session-a",
                        .windows = {{.offset = 0, .limit = 100}},
                    },
            },
    };

    const auto plan = javelin::gui::shell::planSearchTabRestore(std::move(persisted));

    CHECK(plan.accountId == "account-a");
    CHECK(plan.criteria.subject == std::optional<std::string>{"report"});
    CHECK(plan.restored.mode == javelin::app::SearchMode::Promoting);
    CHECK(plan.restored.sessionId == "session-a");
    CHECK(plan.restored.windows.size() == 1);
    CHECK(plan.selection.emailId == std::optional<std::string>{"email-a"});
    CHECK(plan.selection.selectedEmailIds.empty());
}
