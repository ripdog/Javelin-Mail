#pragma once

#include "app/MessageListMaterializationPort.h"
#include "app/MessageListSession.h"
#include "app/MessageListSessionFactory.h"
#include "app/RefreshGeneration.h"
#include "jmap/query/EmailListSort.h"
#include "jmap/search/EmailSearch.h"

#include <QObject>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace javelin::app
{
    class MailApplicationEventsPort;

    class SearchSession final : public MessageListSession
    {
      public:
        SearchSession(std::string accountId, javelin::jmap::search::EmailSearchCriteria criteria,
                      javelin::jmap::query::EmailListSort sort, QString databasePath,
                      MessageListMaterializationPort& materializationPort,
                      MailApplicationEventsPort& events, std::size_t windowSize,
                      std::optional<RestoredSearchState> restored = std::nullopt,
                      QObject* parent = nullptr);

        [[nodiscard]] const std::string& accountId() const override;
        [[nodiscard]] const std::string& query() const;
        [[nodiscard]] const javelin::jmap::search::EmailSearchCriteria& criteria() const;
        [[nodiscard]] QString title() const override;
        [[nodiscard]] const MessageListState& state() const override;
        [[nodiscard]] SearchMode mode() const;
        [[nodiscard]] bool canPromoteToOnline() const;
        [[nodiscard]] const std::string& sessionId() const;

        void loadCachedState(bool forceReload = false) override;
        void refresh(MessageListRefreshMode mode = MessageListRefreshMode::Materialize) override;
        void promoteToOnline();
        void close();
        void refreshAfterMutation();
        void markStale() override;
        void setSort(javelin::jmap::query::EmailListSort sort) override;
        [[nodiscard]] bool canLoadMore() const override;
        [[nodiscard]] bool loadMore() override;
        void ensureThreadMaterialized(std::string threadId) override;
        [[nodiscard]] std::vector<MessageListWindowRequest> windowRequests() const;

      private:
        void startLocalSnapshot();
        void applyLocalVisibleRange();
        void requestOnlineInitial();
        void requestOnlineContinuation(std::size_t offset, std::string anchor);
        void reloadProjectedWindows();
        void
        rebuildFromProjectedWindows(std::vector<javelin::jmap::cache::SearchWindowPage> windows);
        void prefetchNextOnlineWindow();
        void resetOnlineWindows();
        [[nodiscard]] std::size_t nextOnlineOffset() const;
        [[nodiscard]] std::string onlineWindowKey() const;
        [[nodiscard]] bool updateThreadMaterializationState();

        std::string m_accountId;
        std::string m_query;
        javelin::jmap::search::EmailSearchCriteria m_criteria;
        javelin::jmap::query::EmailListSort m_sort;
        QString m_databasePath;
        MessageListMaterializationPort& m_materializationPort;
        MailApplicationEventsPort& m_events;
        std::size_t m_windowSize;
        MessageListState m_state;
        SearchMode m_mode = SearchMode::Local;
        std::string m_sessionId;
        std::vector<javelin::jmap::cache::MessageListItem> m_localSnapshot;
        std::size_t m_localVisibleCount = 0;
        std::vector<MessageListWindow> m_windows;
        std::unordered_set<std::size_t> m_prefetchOffsets;
        std::optional<std::size_t> m_pendingLoadMoreOffset;
        std::optional<std::string> m_pendingLoadMoreAnchor;
        bool m_pendingLoadMoreCommitted = false;
        bool m_pendingLoadMoreRequestCompleted = false;
        bool m_localSnapshotLoaded = false;
        bool m_localSearchInFlight = false;
        bool m_refreshAfterCurrent = false;
        bool m_closed = false;
        bool m_projectedReloadInFlight = false;
        bool m_projectedReloadPending = false;
        bool m_refreshAwaitingCache = false;
        bool m_endReached = false;
        std::uint64_t m_generation = 0;
        std::uint64_t m_refreshRequestId = 0;
        std::uint64_t m_itemsRevision = 0;
        std::uint64_t m_cacheEpoch = 0;
        RefreshGeneration m_refreshGeneration;
        std::unordered_set<std::string> m_materializingThreadIds;
    };

} // namespace javelin::app
