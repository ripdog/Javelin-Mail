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
        [[nodiscard]] bool loadMorePending(std::size_t offset) const;
        [[nodiscard]] std::vector<MessageListWindow>::iterator pendingLoadMoreWindow();
        void finishLoadMore();
        void appendPendingWindow(std::size_t offset);

        // An online load more first tries the cache (including a prefetch already in flight for
        // its offset) and only then commits to an anchored continuation request.
        struct PendingLoadMore
        {
            enum class Phase
            {
                // A prefetch for this offset is in flight; no window is appended yet.
                AwaitingPrefetch,
                // A cached or prefetched page may satisfy it; otherwise request a continuation.
                ReadingCache,
                // The continuation request is in flight; a current cached page may be shown.
                AwaitingContinuation,
                // The continuation landed, so the next cache read must produce the page or fail.
                ReadingContinuation,
            };

            std::size_t offset = 0;
            std::string anchorEmailId;
            Phase phase = Phase::ReadingCache;

            [[nodiscard]] bool committed() const
            {
                return phase == Phase::AwaitingContinuation || phase == Phase::ReadingContinuation;
            }
        };

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
        std::optional<PendingLoadMore> m_pendingLoadMore;
        bool m_localSnapshotLoaded = false;
        bool m_localSearchInFlight = false;
        bool m_refreshAfterCurrent = false;
        bool m_closed = false;
        ProjectedReloadState m_projectedReload = ProjectedReloadState::Idle;
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
