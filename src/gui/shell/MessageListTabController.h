#pragma once

#include "app/MailboxSession.h"
#include "app/SearchSession.h"
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

namespace javelin::jmap::cache
{
    class QueryService;
}

namespace javelin::app
{
    class MailApplicationService;
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
        MessageListTabController(javelin::jmap::cache::QueryService& queryService,
                                 javelin::app::MailApplicationService& mailService,
                                 std::size_t pageSize, QObject* sessionParent,
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
        [[nodiscard]] bool loadCachedPage(TabState& tab, bool forceReload = false);
        [[nodiscard]] bool refresh(TabState& tab);
        [[nodiscard]] bool pageStale(const TabState& tab) const;
        void releaseSession(TabState& tab);

      Q_SIGNALS:
        void pageChanged(javelin::app::MessageListSession* session);
        void operationFailed(const javelin::jmap::OperationError& error);

      private:
        [[nodiscard]] std::vector<std::optional<MessageListTabIdentity>>
        identities(const std::vector<TabState>& tabs) const;
        void bind(javelin::app::MessageListSession& session);

        javelin::jmap::cache::QueryService& m_queryService;
        javelin::app::MailApplicationService& m_mailService;
        std::size_t m_pageSize;
        QObject* m_sessionParent = nullptr;
    };
} // namespace javelin::gui::shell
