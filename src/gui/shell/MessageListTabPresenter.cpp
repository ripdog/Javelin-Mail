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
                    m_panePresenter.showPage(header);
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
                    const auto& page = content.session->page();
                    input.title = m_tabBarPresenter.mailboxTitle(content);
                    input.refreshError = page.refreshError;
                    input.refreshInFlight = page.refreshInFlight;
                    javelin::gui::messages::MessageListPageHeader header{};
                    const auto presentationPosition =
                        page.pendingOffset.has_value() && page.installedOffset.has_value()
                            ? *page.installedOffset
                            : page.position;
                    header.title = input.title;
                    header.offset = presentationPosition;
                    header.position = presentationPosition;
                    header.itemCount = page.items.size();
                    header.returnedLimit = page.returnedLimit;
                    header.total = page.total;
                    input.page = std::move(header);
                }
                else if constexpr (std::is_same_v<Content, SearchTabState>)
                {
                    if (content.session == nullptr)
                        return;
                    const auto& page = content.session->page();
                    input.title = content.session->title();
                    input.refreshError = page.refreshError;
                    input.refreshInFlight = page.refreshInFlight;
                    input.localSearch = content.session->mode() == javelin::app::SearchMode::Local;
                    javelin::gui::messages::MessageListPageHeader header{};
                    const auto presentationPosition =
                        page.pendingOffset.has_value() && page.installedOffset.has_value()
                            ? *page.installedOffset
                            : page.position;
                    header.title = input.title;
                    header.offset = presentationPosition;
                    header.position = presentationPosition;
                    header.itemCount = page.items.size();
                    header.returnedLimit = page.returnedLimit;
                    header.total = page.total;
                    header.search = true;
                    header.indexedSearch = input.localSearch;
                    header.canSearchServer = content.session->canPromoteToOnline();
                    input.page = std::move(header);
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
