#pragma once

#include "app/MessageListSessionFactory.h"
#include "gui/shell/MessageListTabPolicy.h"
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

namespace javelin::jmap
{
    struct OperationError;
}

namespace javelin::app
{
    class MessageListSessionFactoryPort;
}

namespace javelin::gui::shell
{
    struct MailboxTabSessionSpec
    {
        std::string accountId;
        std::string mailboxId;
        QString title;
        std::optional<std::string> role;
        javelin::jmap::query::EmailListSort sort;
        std::optional<javelin::app::RestoredMailboxState> restored;
    };

    struct SearchTabSessionSpec
    {
        std::string accountId;
        javelin::jmap::search::EmailSearchCriteria criteria;
        javelin::jmap::query::EmailListSort sort;
        std::optional<javelin::app::RestoredSearchState> restored;
    };

    struct MessageListTabOpenResult
    {
        std::size_t index = 0;
        bool created = false;
    };

    class MessageListTabController final : public QObject
    {
        Q_OBJECT

      public:
        MessageListTabController(javelin::app::MessageListSessionFactoryPort& sessionFactory,
                                 std::size_t windowSize, QObject* sessionParent,
                                 QObject* parent = nullptr);

        [[nodiscard]] MessageListTabOpenResult
        openOrCreateMailbox(std::vector<TabState>& tabs, MailboxTabSessionSpec spec,
                            std::size_t firstReusableIndex = 0);
        [[nodiscard]] MessageListTabOpenResult
        openOrCreateSearch(std::vector<TabState>& tabs, SearchTabSessionSpec spec,
                           std::size_t firstReusableIndex = 0);
        [[nodiscard]] TabState createMailboxTab(MailboxTabSessionSpec spec);
        [[nodiscard]] TabState createSearchTab(SearchTabSessionSpec spec);

        void
        markTabsStaleForAccount(std::vector<TabState>& tabs, std::string_view accountId,
                                std::optional<std::string_view> refreshedMailboxId = std::nullopt);
        void markSearchTabsStaleForAccount(std::vector<TabState>& tabs, std::string_view accountId);
        [[nodiscard]] bool loadCachedState(TabState& tab, bool forceReload = false);
        [[nodiscard]] bool refresh(TabState& tab,
                                   javelin::app::MessageListRefreshMode mode =
                                       javelin::app::MessageListRefreshMode::Materialize);
        [[nodiscard]] bool canLoadMore(const TabState& tab) const;
        [[nodiscard]] bool loadMore(TabState& tab);
        void setSort(std::vector<TabState>& tabs, javelin::jmap::query::EmailListSort sort);
        [[nodiscard]] bool refreshSearchAfterMutation(TabState& tab, std::string_view accountId);
        [[nodiscard]] bool stateStale(const TabState& tab) const;
        [[nodiscard]] bool stateRefreshInFlight(const TabState& tab) const;
        [[nodiscard]] bool ownsSession(const TabState& tab,
                                       const javelin::app::MessageListSession* session) const;
        [[nodiscard]] bool promoteSearch(TabState& tab);
        [[nodiscard]] bool reveal(TabState& tab, std::string emailId);
        void releaseSession(TabState& tab);

      Q_SIGNALS:
        void stateChanged(javelin::app::MessageListSession* session);
        void operationFailed(const javelin::jmap::OperationError& error);

      private:
        [[nodiscard]] std::vector<std::optional<MessageListTabIdentity>>
        identities(const std::vector<TabState>& tabs) const;
        void bind(javelin::app::MessageListSession& session);

        javelin::app::MessageListSessionFactoryPort& m_sessionFactory;
        std::size_t m_windowSize;
        QObject* m_sessionParent = nullptr;
    };
} // namespace javelin::gui::shell
