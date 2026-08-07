#include "gui/shell/MainWindowStateStore.h"

#include <catch2/catch_test_macros.hpp>

#include <QDataStream>
#include <QIODevice>
#include <QVariantMap>

using namespace javelin::gui::shell;

namespace
{
    [[nodiscard]] QByteArray encodeSettings(const QVariantMap& settings)
    {
        QByteArray encoded;
        QDataStream stream{&encoded, QIODeviceBase::WriteOnly};
        stream.setByteOrder(QDataStream::BigEndian);
        stream.setVersion(QDataStream::Qt_6_6);
        stream << settings;
        return encoded;
    }
} // namespace

TEST_CASE("main window state round-trips every tab type")
{
    PersistedMainWindowState expected{
        .geometry = QByteArrayLiteral("geometry"),
        .splitterState = QByteArrayLiteral("splitter"),
        .activeTabIndex = 3,
        .emailListSort =
            {
                .property = javelin::jmap::query::EmailListSortProperty::Subject,
                .direction = javelin::jmap::query::EmailListSortDirection::Ascending,
            },
        .tabs = {},
    };
    expected.tabs.emplace_back(PersistedMailboxTab{
        .common =
            {
                .title = QStringLiteral("Inbox"),
                .selection = {.threadId = "thread", .emailId = "email"},
            },
        .accountId = "account",
        .mailboxId = "inbox",
        .mailboxRole = "inbox",
        .offset = 200,
    });
    expected.tabs.emplace_back(PersistedSearchTab{
        .common =
            {
                .title = QStringLiteral("Search"),
                .selection = {},
            },
        .accountId = "account",
        .search =
            {
                .criteria = {.text = "needle", .from = "sender@example.test"},
                .restored =
                    {
                        .page =
                            {
                                .offset = 20,
                                .installedOffset = std::nullopt,
                                .pendingOffset = std::nullopt,
                                .position = 20,
                                .returnedLimit = 10,
                                .total = 25,
                                .queryState = "query-state",
                                .anchor = std::nullopt,
                                .items = {},
                                .cacheLoaded = true,
                                .refreshInFlight = false,
                                .stale = true,
                                .refreshError = {},
                            },
                        .mode = javelin::app::SearchMode::Online,
                        .sessionId = "search-session",
                    },
            },
    });
    expected.tabs.emplace_back(PersistedComposeTab{
        .common = {.title = QStringLiteral("Compose"), .selection = {}},
        .accountId = "account",
        .composeSessionId = "compose-session",
        .hasUnsavedChanges = true,
    });
    expected.tabs.emplace_back(PersistedContactsTab{
        .common = {.title = QStringLiteral("Contacts"), .selection = {}},
        .view =
            {
                .accountId = "contacts-account",
                .addressBookId = "address-book",
                .contactId = "contact",
                .filter = QStringLiteral("Ada"),
                .sortMode = 1,
                .groupFilterMode = 2,
                .groupId = "group",
                .selectedContactKeys = {"one", "two"},
            },
    });
    expected.tabs.emplace_back(PersistedCalendarTab{
        .common = {.title = QStringLiteral("Calendar"), .selection = {}},
        .displayedMonth = QDate{2026, 7, 1},
    });

    const auto actual = deserializeMainWindowState(serializeMainWindowState(expected), {});

    CHECK(actual.geometry == expected.geometry);
    CHECK(actual.splitterState == expected.splitterState);
    CHECK(actual.activeTabIndex == expected.activeTabIndex);
    CHECK(actual.emailListSort.property == expected.emailListSort.property);
    CHECK(actual.emailListSort.direction == expected.emailListSort.direction);
    REQUIRE(actual.tabs.size() == expected.tabs.size());

    const auto& mailbox = std::get<PersistedMailboxTab>(actual.tabs[0]);
    CHECK(mailbox.accountId == "account");
    CHECK(mailbox.common.selection.threadId == std::optional<std::string>{"thread"});
    CHECK(mailbox.mailboxId == "inbox");
    CHECK(mailbox.mailboxRole == std::optional<std::string>{"inbox"});
    CHECK(mailbox.offset == 200);

    const auto& search = std::get<PersistedSearchTab>(actual.tabs[1]);
    CHECK(search.accountId == "account");
    CHECK(search.search.criteria.text == std::optional<std::string>{"needle"});
    CHECK(search.search.criteria.from == std::optional<std::string>{"sender@example.test"});
    CHECK(search.search.restored.page.offset == 20);
    CHECK(search.search.restored.page.total == std::optional<std::size_t>{25});
    CHECK(search.search.restored.page.queryState == "query-state");
    CHECK(search.search.restored.mode == javelin::app::SearchMode::Online);
    CHECK(search.search.restored.sessionId == "search-session");

    const auto& compose = std::get<PersistedComposeTab>(actual.tabs[2]);
    CHECK(compose.accountId == "account");
    CHECK(compose.composeSessionId == "compose-session");
    CHECK(compose.hasUnsavedChanges);
    const auto& contacts = std::get<PersistedContactsTab>(actual.tabs[3]);
    CHECK(contacts.view.filter == QStringLiteral("Ada"));
    CHECK((contacts.view.selectedContactKeys == std::vector<std::string>{"one", "two"}));
    const auto& calendar = std::get<PersistedCalendarTab>(actual.tabs[4]);
    CHECK((calendar.displayedMonth == QDate{2026, 7, 1}));
}

TEST_CASE("legacy compose workspace state defaults to no unsaved changes")
{
    const QVariantMap settings{
        {QStringLiteral("tabs/size"), 1},
        {QStringLiteral("tabs/1/type"), QStringLiteral("compose")},
        {QStringLiteral("tabs/1/accountId"), QStringLiteral("account")},
        {QStringLiteral("tabs/1/title"), QStringLiteral("Draft")},
        {QStringLiteral("tabs/1/composeSessionId"), QStringLiteral("compose-session")},
    };

    const auto state = deserializeMainWindowState(encodeSettings(settings), {});

    REQUIRE(state.tabs.size() == 1);
    const auto& compose = std::get<PersistedComposeTab>(state.tabs.front());
    CHECK_FALSE(compose.hasUnsavedChanges);
}

TEST_CASE("main window state accepts legacy account keys on workspace tabs")
{
    const QVariantMap settings{
        {QStringLiteral("tabs/size"), 2},
        {QStringLiteral("tabs/1/type"), QStringLiteral("contacts")},
        {QStringLiteral("tabs/1/accountId"), QStringLiteral("legacy-contacts-account")},
        {QStringLiteral("tabs/1/title"), QStringLiteral("Contacts")},
        {QStringLiteral("tabs/2/type"), QStringLiteral("calendar")},
        {QStringLiteral("tabs/2/accountId"), QStringLiteral("legacy-calendar-account")},
        {QStringLiteral("tabs/2/title"), QStringLiteral("Calendar")},
        {QStringLiteral("tabs/2/displayedMonth"), QStringLiteral("2026-08-01")},
    };

    const auto state = deserializeMainWindowState(encodeSettings(settings), {});

    REQUIRE(state.tabs.size() == 2);
    CHECK(std::holds_alternative<PersistedContactsTab>(state.tabs[0]));
    const auto& calendar = std::get<PersistedCalendarTab>(state.tabs[1]);
    CHECK((calendar.displayedMonth == QDate{2026, 8, 1}));
}

TEST_CASE("main window state rejects account-bound tabs without an account")
{
    PersistedMainWindowState state;
    state.tabs.emplace_back(PersistedMailboxTab{
        .common = {.title = QStringLiteral("Inbox"), .selection = {}},
        .accountId = {},
        .mailboxId = "inbox",
        .mailboxRole = "inbox",
        .offset = 0,
    });
    state.tabs.emplace_back(PersistedSearchTab{
        .common = {.title = QStringLiteral("Search"), .selection = {}},
        .accountId = {},
        .search = {},
    });
    state.tabs.emplace_back(PersistedComposeTab{
        .common = {.title = QStringLiteral("Compose"), .selection = {}},
        .accountId = {},
        .composeSessionId = "compose-session",
    });

    CHECK(deserializeMainWindowState(serializeMainWindowState(state), {}).tabs.empty());
}

TEST_CASE("main window state ignores invalid tab records")
{
    const QVariantMap settings{
        {QStringLiteral("tabs/size"), 1},
        {QStringLiteral("tabs/1/type"), QStringLiteral("mailbox")},
        {QStringLiteral("tabs/1/accountId"), QStringLiteral("account")},
    };

    CHECK(deserializeMainWindowState(encodeSettings(settings), {}).tabs.empty());
}

TEST_CASE("main window state falls back safely when the payload is corrupt")
{
    const javelin::jmap::query::EmailListSort fallback{
        .property = javelin::jmap::query::EmailListSortProperty::From,
        .direction = javelin::jmap::query::EmailListSortDirection::Ascending,
    };
    const auto state = deserializeMainWindowState(QByteArrayLiteral("not a data stream"), fallback);

    CHECK(state.tabs.empty());
    CHECK(state.emailListSort.property == fallback.property);
    CHECK(state.emailListSort.direction == fallback.direction);
}
