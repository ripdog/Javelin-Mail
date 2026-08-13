#include "gui/shell/MailWorkspaceController.h"

namespace javelin::gui::shell
{
    MailWorkspaceController::MailWorkspaceController(
        javelin::app::MessageListSessionFactoryPort& sessionFactory, const std::size_t windowSize,
        QObject* sessionParent, QObject* parent)
        : QObject(parent), m_messageListTabs(sessionFactory, windowSize, sessionParent, nullptr)
    {
        connect(&m_messageListTabs, &MessageListTabController::stateChanged, this,
                &MailWorkspaceController::stateChanged);
        connect(&m_messageListTabs, &MessageListTabController::operationFailed, this,
                &MailWorkspaceController::operationFailed);
    }

    std::vector<TabState>& MailWorkspaceController::tabs()
    {
        return m_tabs;
    }

    const std::vector<TabState>& MailWorkspaceController::tabs() const
    {
        return m_tabs;
    }

    std::optional<int>& MailWorkspaceController::activeIndex()
    {
        return m_activeIndex;
    }

    const std::optional<int>& MailWorkspaceController::activeIndex() const
    {
        return m_activeIndex;
    }

    javelin::jmap::query::EmailListSort& MailWorkspaceController::sort()
    {
        return m_sort;
    }

    const javelin::jmap::query::EmailListSort& MailWorkspaceController::sort() const
    {
        return m_sort;
    }

    TabState* MailWorkspaceController::activeTab()
    {
        return activeWorkspaceTab(m_tabs, m_activeIndex);
    }

    const TabState* MailWorkspaceController::activeTab() const
    {
        return activeWorkspaceTab(m_tabs, m_activeIndex);
    }

    MessageListTabController& MailWorkspaceController::messageListTabs()
    {
        return m_messageListTabs;
    }

    bool MailWorkspaceController::ownsActiveSession(
        const javelin::app::MessageListSession* session) const
    {
        const auto* tab = activeTab();
        return tab != nullptr && m_messageListTabs.ownsSession(*tab, session);
    }

    bool MailWorkspaceController::promoteSearch(TabState& tab)
    {
        return m_messageListTabs.promoteSearch(tab);
    }

    void MailWorkspaceController::prepareRestore(const std::size_t tabCount)
    {
        for (auto& tab : m_tabs)
            m_messageListTabs.releaseSession(tab);
        m_tabs.clear();
        m_tabs.reserve(tabCount);
        m_activeIndex.reset();
    }

    int MailWorkspaceController::openMailbox(std::string accountId, std::string mailboxId,
                                             QString title, std::optional<std::string> role,
                                             const std::size_t firstReusableIndex)
    {
        const auto result =
            m_messageListTabs.openOrCreateMailbox(m_tabs,
                                                  {.accountId = std::move(accountId),
                                                   .mailboxId = std::move(mailboxId),
                                                   .title = std::move(title),
                                                   .role = std::move(role),
                                                   .sort = m_sort,
                                                   .restored = std::nullopt},
                                                  firstReusableIndex);
        m_activeIndex = static_cast<int>(result.index);
        return *m_activeIndex;
    }

    int MailWorkspaceController::activateHomeMailbox(std::string accountId, std::string mailboxId,
                                                     QString title, std::optional<std::string> role)
    {
        auto tab = m_messageListTabs.createMailboxTab({.accountId = std::move(accountId),
                                                       .mailboxId = std::move(mailboxId),
                                                       .title = std::move(title),
                                                       .role = std::move(role),
                                                       .sort = m_sort,
                                                       .restored = std::nullopt});
        if (m_tabs.empty())
            m_tabs.push_back(std::move(tab));
        else
        {
            m_messageListTabs.releaseSession(m_tabs[0]);
            m_tabs[0] = std::move(tab);
        }
        m_activeIndex = 0;
        return 0;
    }

    int MailWorkspaceController::openSearch(std::string accountId,
                                            javelin::jmap::search::EmailSearchCriteria criteria,
                                            const std::size_t firstReusableIndex)
    {
        const auto result = m_messageListTabs.openOrCreateSearch(m_tabs,
                                                                 {.accountId = std::move(accountId),
                                                                  .criteria = std::move(criteria),
                                                                  .sort = m_sort,
                                                                  .restored = std::nullopt},
                                                                 firstReusableIndex);
        m_activeIndex = static_cast<int>(result.index);
        return *m_activeIndex;
    }

    int MailWorkspaceController::restoreMailbox(MailboxTabSessionSpec spec)
    {
        spec.sort = m_sort;
        m_tabs.push_back(m_messageListTabs.createMailboxTab(std::move(spec)));
        return static_cast<int>(m_tabs.size() - 1);
    }

    int MailWorkspaceController::restoreSearch(SearchTabSessionSpec spec)
    {
        spec.sort = m_sort;
        m_tabs.push_back(m_messageListTabs.createSearchTab(std::move(spec)));
        return static_cast<int>(m_tabs.size() - 1);
    }

    void MailWorkspaceController::setActiveIndex(std::optional<int> index)
    {
        if (index.has_value() && (*index < 0 || static_cast<std::size_t>(*index) >= m_tabs.size()))
            index.reset();
        m_activeIndex = index;
    }

    bool MailWorkspaceController::eraseTab(const int index)
    {
        if (index < 0 || static_cast<std::size_t>(index) >= m_tabs.size())
            return false;
        m_messageListTabs.releaseSession(m_tabs[static_cast<std::size_t>(index)]);
        m_activeIndex = activeTabIndexAfterClose(m_tabs.size(), m_activeIndex, index);
        m_tabs.erase(m_tabs.begin() + index);
        return true;
    }

    void MailWorkspaceController::markTabsStaleForAccount(
        const std::string_view accountId, const std::optional<std::string_view> refreshedMailboxId)
    {
        m_messageListTabs.markTabsStaleForAccount(m_tabs, accountId, refreshedMailboxId);
    }

    void MailWorkspaceController::markSearchTabsStaleForAccount(const std::string_view accountId)
    {
        m_messageListTabs.markSearchTabsStaleForAccount(m_tabs, accountId);
    }

    void MailWorkspaceController::setSort(javelin::jmap::query::EmailListSort sort)
    {
        m_sort = std::move(sort);
        m_messageListTabs.setSort(m_tabs, m_sort);
    }

    bool MailWorkspaceController::loadCachedState(TabState& tab, const bool forceReload)
    {
        return m_messageListTabs.loadCachedState(tab, forceReload);
    }

    bool MailWorkspaceController::refresh(TabState& tab,
                                          const javelin::app::MessageListRefreshMode mode)
    {
        return m_messageListTabs.refresh(tab, mode);
    }

    bool MailWorkspaceController::refresh(const std::size_t tabIndex,
                                          const javelin::app::MessageListRefreshMode mode)
    {
        return tabIndex < m_tabs.size() && m_messageListTabs.refresh(m_tabs[tabIndex], mode);
    }

    bool MailWorkspaceController::canLoadMore(const TabState& tab) const
    {
        return m_messageListTabs.canLoadMore(tab);
    }

    bool MailWorkspaceController::loadMore(TabState& tab)
    {
        return m_messageListTabs.loadMore(tab);
    }

    void MailWorkspaceController::releaseSession(TabState& tab)
    {
        m_messageListTabs.releaseSession(tab);
    }

    bool MailWorkspaceController::stateStale(const TabState& tab) const
    {
        return m_messageListTabs.stateStale(tab);
    }
} // namespace javelin::gui::shell
