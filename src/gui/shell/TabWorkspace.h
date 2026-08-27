#pragma once

#include <QString>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace javelin::app
{
    class MailboxSession;
    class MessageListSession;
    class SearchSession;
} // namespace javelin::app

namespace javelin::gui::compose
{
    class ComposeTabWidget;
}

namespace javelin::gui::contacts
{
    class ContactsManagerWidget;
}

namespace javelin::gui::calendar
{
    class MonthCalendarWidget;
}

namespace javelin::gui::shell
{

    struct TabSelectionState
    {
        std::optional<std::string> threadId;
        std::optional<std::string> emailId;
        std::vector<std::string> selectedEmailIds;
    };

    struct MailboxTabState
    {
        javelin::app::MailboxSession* session = nullptr;
        TabSelectionState selection;
        std::vector<std::string> expandedThreadIds;
    };

    struct SearchTabState
    {
        javelin::app::SearchSession* session = nullptr;
        TabSelectionState selection;
        std::vector<std::string> expandedThreadIds;
    };

    struct ComposeTabState
    {
        std::string accountId;
        std::string composeSessionId;
        QString title;
        javelin::gui::compose::ComposeTabWidget* widget = nullptr;
        TabSelectionState selection;
    };

    struct ContactsTabState
    {
        QString title;
        javelin::gui::contacts::ContactsManagerWidget* widget = nullptr;
        TabSelectionState selection;
    };

    struct CalendarTabState
    {
        QString title;
        javelin::gui::calendar::MonthCalendarWidget* widget = nullptr;
        TabSelectionState selection;
    };

    using TabContent = std::variant<MailboxTabState, SearchTabState, ComposeTabState,
                                    ContactsTabState, CalendarTabState>;

    struct TabState
    {
        TabContent content;
    };

    enum class TabKind
    {
        Mailbox,
        Search,
        Compose,
        Contacts,
        Calendar,
    };

    [[nodiscard]] const TabState* activeWorkspaceTab(const std::vector<TabState>& tabs,
                                                     std::optional<int> activeIndex);
    [[nodiscard]] TabState* activeWorkspaceTab(std::vector<TabState>& tabs,
                                               std::optional<int> activeIndex);
    [[nodiscard]] TabKind tabKind(const TabState& tab);
    [[nodiscard]] bool tabCanClose(const TabState& tab, std::size_t index);
    [[nodiscard]] std::optional<std::string> tabAccountId(const TabState& tab);
    [[nodiscard]] std::optional<std::string> tabMailboxId(const TabState& tab);
    [[nodiscard]] const javelin::app::MessageListSession* messageListSession(const TabState& tab);
    [[nodiscard]] javelin::app::MessageListSession* messageListSession(TabState& tab);
    [[nodiscard]] const TabSelectionState& tabSelection(const TabState& tab);
    [[nodiscard]] TabSelectionState& tabSelection(TabState& tab);
    [[nodiscard]] std::optional<int> activeTabIndexAfterClose(std::size_t tabCountBeforeClose,
                                                              std::optional<int> activeIndex,
                                                              int closedIndex);
    [[nodiscard]] std::optional<int> activeTabIndexAfterMove(std::optional<int> activeIndex,
                                                             int fromIndex, int toIndex);

} // namespace javelin::gui::shell
