#include "gui/shell/MessageListTabPresenter.h"

#include "app/MailboxSession.h"
#include "app/SearchSession.h"
#include "gui/messages/MessageListPanePresenter.h"
#include "gui/shell/TabBarPresenter.h"

#include <type_traits>

namespace javelin::gui::shell
{
    MessageListTabPresenter::MessageListTabPresenter(
        javelin::gui::messages::MessageListPanePresenter& panePresenter,
        const TabBarPresenter& tabBarPresenter)
        : m_panePresenter(panePresenter), m_tabBarPresenter(tabBarPresenter)
    {
    }

    void MessageListTabPresenter::showEmptyState(const TabState* tab,
                                                 const std::size_t itemCount) const
    {
        m_panePresenter.showEmptyState(
            planMessageListPresentation(inputFor(tab, itemCount)).emptyState);
    }

    void MessageListTabPresenter::showHeader(const TabState* tab) const
    {
        const auto plan = planMessageListPresentation(inputFor(tab, 0));
        std::visit(
            [this](const auto& header)
            {
                using Header = std::decay_t<decltype(header)>;
                if constexpr (std::is_same_v<Header, std::monostate>)
                    m_panePresenter.showNoContext();
                else if constexpr (std::is_same_v<Header,
                                                  javelin::gui::messages::MessageListContextHeader>)
                    m_panePresenter.showContext(header);
                else
                    m_panePresenter.showList(header);
            },
            plan.header);
    }

    MessageListPresentationInput
    MessageListTabPresenter::inputFor(const TabState* tab, const std::size_t itemCount) const
    {
        MessageListPresentationInput input{};
        input.itemCount = itemCount;
        if (tab == nullptr)
            return input;

        input.tabKind = tabKind(*tab);
        std::visit(
            [this, &input](const auto& content)
            {
                using Content = std::decay_t<decltype(content)>;
                if constexpr (std::is_same_v<Content, MailboxTabState>)
                {
                    if (content.session == nullptr)
                        return;
                    const auto& state = content.session->state();
                    input.title = m_tabBarPresenter.mailboxTitle(content);
                    input.itemCount = state.items.size();
                    input.refreshError = state.refreshError;
                    input.refreshInFlight = state.refreshInFlight;
                    input.list = javelin::gui::messages::MessageListHeader{
                        .title = input.title,
                        .itemCount = state.items.size(),
                        .total = state.total,
                        .search = false,
                        .indexedSearch = false,
                        .canSearchServer = false,
                        .refreshInFlight = state.refreshInFlight,
                        .loadMoreInFlight = state.loadMoreInFlight,
                        .loadMoreError = state.loadMoreError,
                    };
                }
                else if constexpr (std::is_same_v<Content, SearchTabState>)
                {
                    if (content.session == nullptr)
                        return;
                    const auto& state = content.session->state();
                    input.title = content.session->title();
                    input.itemCount = state.items.size();
                    input.refreshError = state.refreshError;
                    input.refreshInFlight = state.refreshInFlight;
                    input.localSearch = content.session->mode() == javelin::app::SearchMode::Local;
                    input.list = javelin::gui::messages::MessageListHeader{
                        .title = input.title,
                        .itemCount = state.items.size(),
                        .total = state.total,
                        .search = true,
                        .indexedSearch = input.localSearch,
                        .canSearchServer = content.session->canPromoteToOnline(),
                        .refreshInFlight = state.refreshInFlight,
                        .loadMoreInFlight = state.loadMoreInFlight,
                        .loadMoreError = state.loadMoreError,
                    };
                }
                else
                {
                    input.title = content.title;
                }
            },
            tab->content);
        return input;
    }
} // namespace javelin::gui::shell
