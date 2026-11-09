#pragma once

#include "app/MessageListMaterializationPort.h"
#include "app/MessageListSession.h"
#include "app/MessageListSessionFactory.h"
#include "app/RefreshGeneration.h"
#include "jmap/query/EmailListSort.h"
#include "jmap/search/EmailSearch.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace javelin::app
{
    class MailApplicationEventsPort;

    class MailboxSession final : public MessageListSession
    {
      public:
        MailboxSession(std::string accountId, std::string mailboxId, QString title,
                       std::optional<std::string> role, javelin::jmap::query::EmailListSort sort,
                       QString databasePath, MessageListMaterializationPort& materializationPort,
                       std::size_t windowSize, MailApplicationEventsPort& events,
                       std::optional<RestoredMailboxState> restored = std::nullopt,
                       QObject* parent = nullptr);
        ~MailboxSession() override;

        [[nodiscard]] const std::string& accountId() const override;
        [[nodiscard]] const std::string& mailboxId() const;
        [[nodiscard]] QString title() const override;
        [[nodiscard]] const std::optional<std::string>& role() const;
        [[nodiscard]] const MessageListState& state() const override;
        void updateMetadata(QString title, std::optional<std::string> role);

        void loadCachedState(bool forceReload = false) override;
        void refresh(MessageListRefreshMode mode = MessageListRefreshMode::Materialize) override;
        void markStale() override;
        void setSort(javelin::jmap::query::EmailListSort sort) override;
        void setQuickFilter(javelin::jmap::search::EmailSearchCriteria criteria);
        void setQuickFilterContinuitySelection(std::optional<std::string> emailId,
                                               std::optional<std::string> threadId);
        [[nodiscard]] const javelin::jmap::search::EmailSearchCriteria& quickFilter() const;
        [[nodiscard]] bool quickFilterActive() const;
        void reveal(std::string emailId);
        [[nodiscard]] bool canLoadMore() const override;
        [[nodiscard]] bool loadMore() override;
        void ensureThreadMaterialized(std::string threadId) override;
        [[nodiscard]] std::vector<MessageListWindowRequest> windowRequests() const;

      private:
        void reloadProjectedWindows();
        void requestInitialWindow(MessageListRefreshMode mode, std::optional<std::string> anchor,
                                  std::int64_t anchorOffset);
        void rebuildFromProjectedWindows(
            std::vector<javelin::jmap::cache::MailboxWindowPage> windows,
            std::optional<javelin::jmap::cache::MessageListItem> continuityItem = std::nullopt);
        void resetToInitialWindow();
        [[nodiscard]] std::string queryKey() const;
        [[nodiscard]] std::string quickFilterWindowKey() const;
        [[nodiscard]] javelin::jmap::search::EmailSearchCriteria filteredCriteria() const;
        [[nodiscard]] std::size_t nextOffset() const;
        [[nodiscard]] bool updateThreadMaterializationState();
        [[nodiscard]] bool loadMorePending(std::size_t offset) const;
        [[nodiscard]] std::vector<MessageListWindow>::iterator pendingLoadMoreWindow();
        void finishLoadMore();
        void abandonLoadMore();

        // A load more owns the trailing window it appended until that window's page is applied.
        struct PendingLoadMore
        {
            enum class Phase
            {
                // The query request is in flight; a current cached page may already be shown.
                AwaitingSummary,
                // The request landed, so the next cache read must produce the page or fail.
                ReadingPage,
            };

            std::size_t offset = 0;
            Phase phase = Phase::AwaitingSummary;
        };

        std::string m_accountId;
        std::string m_mailboxId;
        QString m_title;
        std::optional<std::string> m_role;
        javelin::jmap::query::EmailListSort m_sort;
        javelin::jmap::search::EmailSearchCriteria m_quickFilter;
        std::string m_quickFilterSessionId;
        std::optional<std::string> m_quickFilterContinuityEmailId;
        std::optional<std::string> m_quickFilterContinuityThreadId;
        std::optional<std::size_t> m_quickFilterContinuityPreferredIndex;
        std::optional<std::string> m_quickFilterAuthoritativeAnchorEmailId;
        bool m_quickFilterContinuityApplied = false;
        QString m_databasePath;
        MessageListMaterializationPort& m_materializationPort;
        MailApplicationEventsPort& m_events;
        std::size_t m_windowSize;
        MessageListState m_state;
        std::vector<MessageListWindow> m_windows;
        MailboxObservationLease m_observation;
        std::optional<PendingLoadMore> m_pendingLoadMore;
        std::uint64_t m_generation = 0;
        std::uint64_t m_refreshRequestId = 0;
        std::uint64_t m_itemsRevision = 0;
        std::uint64_t m_cacheEpoch = 0;
        RefreshGeneration m_refreshGeneration;
        ProjectedReloadState m_projectedReload = ProjectedReloadState::Idle;
        bool m_refreshAwaitingCache = false;
        bool m_endReached = false;
        std::unordered_set<std::string> m_materializingThreadIds;
    };
} // namespace javelin::app
