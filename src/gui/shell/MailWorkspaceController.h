#pragma once

#include "gui/shell/MessageListTabController.h"
#include "gui/shell/TabWorkspace.h"
#include "jmap/query/EmailListSort.h"
#include "jmap/search/EmailSearch.h"

#include <QObject>
#include <QString>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace javelin::app
{
    class MessageListSessionFactoryPort;
}

namespace javelin::gui::shell
{
    class MailWorkspaceController final : public QObject
    {
        Q_OBJECT

      public:
        MailWorkspaceController(javelin::app::MessageListSessionFactoryPort& sessionFactory,
                                std::size_t windowSize, QObject* sessionParent,
                                QObject* parent = nullptr);

        [[nodiscard]] std::vector<TabState>& tabs();
        [[nodiscard]] const std::vector<TabState>& tabs() const;
        [[nodiscard]] std::optional<int>& activeIndex();
        [[nodiscard]] const std::optional<int>& activeIndex() const;
        [[nodiscard]] javelin::jmap::query::EmailListSort& sort();
        [[nodiscard]] const javelin::jmap::query::EmailListSort& sort() const;
        [[nodiscard]] TabState* activeTab();
        [[nodiscard]] const TabState* activeTab() const;
        [[nodiscard]] MessageListTabController& messageListTabs();
        [[nodiscard]] bool ownsActiveSession(const javelin::app::MessageListSession* session) const;
        [[nodiscard]] bool promoteSearch(TabState& tab);
        void prepareRestore(std::size_t tabCount);

        [[nodiscard]] int openMailbox(std::string accountId, std::string mailboxId, QString title,
                                      std::optional<std::string> role,
                                      std::size_t firstReusableIndex = 0);
        [[nodiscard]] int activateHomeMailbox(std::string accountId, std::string mailboxId,
                                              QString title, std::optional<std::string> role);
        [[nodiscard]] int openSearch(std::string accountId,
                                     javelin::jmap::search::EmailSearchCriteria criteria,
                                     std::size_t firstReusableIndex = 0);
        [[nodiscard]] int restoreMailbox(MailboxTabSessionSpec spec);
        [[nodiscard]] int restoreSearch(SearchTabSessionSpec spec);

        void setActiveIndex(std::optional<int> index);
        [[nodiscard]] bool moveTab(int fromIndex, int toIndex);
        [[nodiscard]] bool eraseTab(int index);
        void
        markTabsStaleForAccount(std::string_view accountId,
                                std::optional<std::string_view> refreshedMailboxId = std::nullopt);
        void markSearchTabsStaleForAccount(std::string_view accountId);
        void setSort(javelin::jmap::query::EmailListSort sort);
        [[nodiscard]] bool loadCachedState(TabState& tab, bool forceReload = false);
        [[nodiscard]] bool refresh(TabState& tab, javelin::app::MessageListRefreshMode mode);
        [[nodiscard]] bool refresh(std::size_t tabIndex, javelin::app::MessageListRefreshMode mode);
        [[nodiscard]] bool canLoadMore(const TabState& tab) const;
        [[nodiscard]] bool loadMore(TabState& tab);
        void releaseSession(TabState& tab);
        [[nodiscard]] bool stateStale(const TabState& tab) const;

      Q_SIGNALS:
        void stateChanged(javelin::app::MessageListSession* session);
        void operationFailed(const javelin::jmap::OperationError& error);

      private:
        std::vector<TabState> m_tabs;
        std::optional<int> m_activeIndex;
        javelin::jmap::query::EmailListSort m_sort;
        MessageListTabController m_messageListTabs;
    };
} // namespace javelin::gui::shell
