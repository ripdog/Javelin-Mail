#include "gui/shell/TabWorkspace.h"

#include "app/MailboxSession.h"
#include "app/MessageListSession.h"
#include "app/SearchSession.h"

#include <type_traits>

namespace javelin::gui::shell
{

    std::optional<std::string> tabAccountId(const TabState& tab)
    {
        return std::visit(
            [](const auto& content) -> std::optional<std::string>
            {
                using Content = std::decay_t<decltype(content)>;
                if constexpr (std::is_same_v<Content, MailboxTabState> ||
                              std::is_same_v<Content, SearchTabState>)
                {
                    return content.session == nullptr
                               ? std::optional<std::string>{std::nullopt}
                               : std::optional<std::string>{content.session->accountId()};
                }
                else if constexpr (std::is_same_v<Content, ComposeTabState>)
                {
                    return content.accountId.empty()
                               ? std::optional<std::string>{std::nullopt}
                               : std::optional<std::string>{content.accountId};
                }
                else
                {
                    return std::nullopt;
                }
            },
            tab.content);
    }

    std::optional<std::string> tabMailboxId(const TabState& tab)
    {
        const auto* mailbox = std::get_if<MailboxTabState>(&tab.content);
        if (mailbox == nullptr || mailbox->session == nullptr)
        {
            return std::nullopt;
        }
        return mailbox->session->mailboxId();
    }

    const javelin::app::MessageListSession* messageListSession(const TabState& tab)
    {
        return std::visit(
            [](const auto& content) -> const javelin::app::MessageListSession*
            {
                using Content = std::decay_t<decltype(content)>;
                if constexpr (std::is_same_v<Content, MailboxTabState> ||
                              std::is_same_v<Content, SearchTabState>)
                {
                    return content.session;
                }
                return nullptr;
            },
            tab.content);
    }

    javelin::app::MessageListSession* messageListSession(TabState& tab)
    {
        return std::visit(
            [](auto& content) -> javelin::app::MessageListSession*
            {
                using Content = std::decay_t<decltype(content)>;
                if constexpr (std::is_same_v<Content, MailboxTabState> ||
                              std::is_same_v<Content, SearchTabState>)
                {
                    return content.session;
                }
                return nullptr;
            },
            tab.content);
    }

    const std::vector<std::string>* tabExpandedThreadIds(const TabState& tab)
    {
        return std::visit(
            [](const auto& content) -> const std::vector<std::string>*
            {
                using Content = std::decay_t<decltype(content)>;
                if constexpr (std::is_same_v<Content, MailboxTabState> ||
                              std::is_same_v<Content, SearchTabState>)
                {
                    return &content.expandedThreadIds;
                }
                return nullptr;
            },
            tab.content);
    }

    std::vector<std::string>* tabExpandedThreadIds(TabState& tab)
    {
        return std::visit(
            [](auto& content) -> std::vector<std::string>*
            {
                using Content = std::decay_t<decltype(content)>;
                if constexpr (std::is_same_v<Content, MailboxTabState> ||
                              std::is_same_v<Content, SearchTabState>)
                {
                    return &content.expandedThreadIds;
                }
                return nullptr;
            },
            tab.content);
    }

} // namespace javelin::gui::shell
