#include "gui/shell/TabWorkspace.h"

#include <algorithm>
#include <type_traits>

namespace javelin::gui::shell
{

    const TabState* activeWorkspaceTab(const std::vector<TabState>& tabs,
                                       const std::optional<int> activeIndex)
    {
        if (!activeIndex.has_value() || *activeIndex < 0 ||
            static_cast<std::size_t>(*activeIndex) >= tabs.size())
        {
            return nullptr;
        }
        return &tabs[static_cast<std::size_t>(*activeIndex)];
    }

    TabState* activeWorkspaceTab(std::vector<TabState>& tabs, const std::optional<int> activeIndex)
    {
        if (!activeIndex.has_value() || *activeIndex < 0 ||
            static_cast<std::size_t>(*activeIndex) >= tabs.size())
        {
            return nullptr;
        }
        return &tabs[static_cast<std::size_t>(*activeIndex)];
    }

    TabKind tabKind(const TabState& tab)
    {
        return std::visit(
            [](const auto& content)
            {
                using Content = std::decay_t<decltype(content)>;
                if constexpr (std::is_same_v<Content, MailboxTabState>)
                {
                    return TabKind::Mailbox;
                }
                else if constexpr (std::is_same_v<Content, SearchTabState>)
                {
                    return TabKind::Search;
                }
                else if constexpr (std::is_same_v<Content, ComposeTabState>)
                {
                    return TabKind::Compose;
                }
                else if constexpr (std::is_same_v<Content, ContactsTabState>)
                {
                    return TabKind::Contacts;
                }
                else
                {
                    return TabKind::Calendar;
                }
            },
            tab.content);
    }

    bool tabCanClose(const TabState& tab, const std::size_t index)
    {
        const auto kind = tabKind(tab);
        return index != 0 || kind == TabKind::Compose || kind == TabKind::Contacts ||
               kind == TabKind::Calendar;
    }

    const TabSelectionState& tabSelection(const TabState& tab)
    {
        return std::visit([](const auto& content) -> const TabSelectionState&
                          { return content.selection; }, tab.content);
    }

    TabSelectionState& tabSelection(TabState& tab)
    {
        return std::visit([](auto& content) -> TabSelectionState& { return content.selection; },
                          tab.content);
    }

    std::optional<int> activeTabIndexAfterClose(const std::size_t tabCountBeforeClose,
                                                const std::optional<int> activeIndex,
                                                const int closedIndex)
    {
        if (closedIndex < 0 || static_cast<std::size_t>(closedIndex) >= tabCountBeforeClose)
        {
            return activeIndex;
        }
        if (tabCountBeforeClose <= 1)
        {
            return std::nullopt;
        }
        if (!activeIndex.has_value())
        {
            return std::max(0, closedIndex - 1);
        }
        if (*activeIndex < closedIndex)
        {
            return activeIndex;
        }
        if (*activeIndex > closedIndex)
        {
            return *activeIndex - 1;
        }
        return std::max(0, closedIndex - 1);
    }

    std::optional<int> activeTabIndexAfterMove(const std::optional<int> activeIndex,
                                               const int fromIndex, const int toIndex)
    {
        if (!activeIndex.has_value() || fromIndex < 0 || toIndex < 0 || fromIndex == toIndex)
            return activeIndex;
        if (*activeIndex == fromIndex)
            return toIndex;
        if (fromIndex < toIndex && *activeIndex > fromIndex && *activeIndex <= toIndex)
            return *activeIndex - 1;
        if (toIndex < fromIndex && *activeIndex >= toIndex && *activeIndex < fromIndex)
            return *activeIndex + 1;
        return activeIndex;
    }

} // namespace javelin::gui::shell
