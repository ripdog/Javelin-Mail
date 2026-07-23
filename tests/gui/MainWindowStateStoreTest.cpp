#include "gui/shell/MainWindowStateStore.h"

#include <catch2/catch_test_macros.hpp>

#include <QSettings>
#include <QTemporaryDir>

using namespace javelin::gui::shell;

TEST_CASE("main window state round-trips every tab type")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    QSettings settings{directory.filePath(QStringLiteral("window.ini")), QSettings::IniFormat};

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
                .accountId = "account",
                .title = QStringLiteral("Inbox"),
                .selection = {.threadId = "thread", .emailId = "email"},
            },
        .mailboxId = "inbox",
        .mailboxRole = "inbox",
        .offset = 200,
    });
    expected.tabs.emplace_back(PersistedSearchTab{
        .common =
            {
                .accountId = "account",
                .title = QStringLiteral("Search"),
                .selection = {},
            },
        .search =
            {
                .criteria = {.text = "needle", .from = "sender@example.test"},
                .restored =
                    {
                        .page =
                            {
                                .offset = 20,
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
        .common = {.accountId = "account", .title = QStringLiteral("Compose"), .selection = {}},
        .composeSessionId = "compose-session",
    });
    expected.tabs.emplace_back(PersistedContactsTab{
        .common = {.accountId = "account", .title = QStringLiteral("Contacts"), .selection = {}},
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
        .common = {.accountId = "account", .title = QStringLiteral("Calendar"), .selection = {}},
        .displayedMonth = QDate{2026, 7, 1},
    });

    writeMainWindowState(settings, expected);
    settings.sync();
    auto actual = readMainWindowState(settings, {});

    CHECK(actual.geometry == expected.geometry);
    CHECK(actual.splitterState == expected.splitterState);
    CHECK(actual.activeTabIndex == expected.activeTabIndex);
    CHECK(actual.emailListSort.property == expected.emailListSort.property);
    CHECK(actual.emailListSort.direction == expected.emailListSort.direction);
    REQUIRE(actual.tabs.size() == expected.tabs.size());

    const auto& mailbox = std::get<PersistedMailboxTab>(actual.tabs[0]);
    CHECK(mailbox.common.accountId == "account");
    CHECK(mailbox.common.selection.threadId == std::optional<std::string>{"thread"});
    CHECK(mailbox.mailboxId == "inbox");
    CHECK(mailbox.mailboxRole == std::optional<std::string>{"inbox"});
    CHECK(mailbox.offset == 200);

    const auto& search = std::get<PersistedSearchTab>(actual.tabs[1]);
    CHECK(search.search.criteria.text == std::optional<std::string>{"needle"});
    CHECK(search.search.criteria.from == std::optional<std::string>{"sender@example.test"});
    CHECK(search.search.restored.page.offset == 20);
    CHECK(search.search.restored.page.total == std::optional<std::size_t>{25});
    CHECK(search.search.restored.page.queryState == "query-state");
    CHECK(search.search.restored.mode == javelin::app::SearchMode::Online);
    CHECK(search.search.restored.sessionId == "search-session");

    CHECK(std::get<PersistedComposeTab>(actual.tabs[2]).composeSessionId == "compose-session");
    const auto& contacts = std::get<PersistedContactsTab>(actual.tabs[3]);
    CHECK(contacts.view.filter == QStringLiteral("Ada"));
    CHECK((contacts.view.selectedContactKeys == std::vector<std::string>{"one", "two"}));
    CHECK((std::get<PersistedCalendarTab>(actual.tabs[4]).displayedMonth == QDate{2026, 7, 1}));
}

TEST_CASE("main window state ignores invalid tab records")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    QSettings settings{directory.filePath(QStringLiteral("window.ini")), QSettings::IniFormat};
    settings.beginGroup(QStringLiteral("mainWindow"));
    settings.beginWriteArray(QStringLiteral("tabs"));
    settings.setArrayIndex(0);
    settings.setValue(QStringLiteral("type"), QStringLiteral("mailbox"));
    settings.setValue(QStringLiteral("accountId"), QStringLiteral("account"));
    settings.endArray();
    settings.endGroup();

    CHECK(readMainWindowState(settings, {}).tabs.empty());
}
