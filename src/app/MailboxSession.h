#pragma once

#include "app/MessageListMaterializationPort.h"
#include "app/MessageListSession.h"
#include "app/MessageListSessionFactory.h"
#include "app/RefreshGeneration.h"
#include "jmap/cache/QueryReader.h"
#include "jmap/query/EmailListSort.h"
#include "jmap/search/EmailSearch.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace javelin::app
{
    class MailApplicationEventsPort;

    class MailboxSession final : public MessageListSession
    {
      public:
        MailboxSession(std::string accountId, std::string mailboxId, QString title,
                       std::optional<std::string> role, javelin::jmap::query::EmailListSort sort,
                       javelin::jmap::cache::QueryReader& queryReader,
                       MessageListMaterializationPort& materializationPort, std::size_t windowSize,
                       MailApplicationEventsPort& events,
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
        [[nodiscard]] const javelin::jmap::search::EmailSearchCriteria& quickFilter() const;
        [[nodiscard]] bool quickFilterActive() const;
        void reveal(std::string emailId);
        [[nodiscard]] bool canLoadMore() const override;
        [[nodiscard]] bool loadMore() override;
        [[nodiscard]] std::vector<MessageListWindowRequest> windowRequests() const;

      private:
        void reloadProjectedWindows();
        void requestInitialWindow(MessageListRefreshMode mode, std::optional<std::string> anchor,
                                  std::int64_t anchorOffset);
        void
        rebuildFromProjectedWindows(std::vector<javelin::jmap::cache::MailboxWindowPage> windows);
        void resetToInitialWindow();
        [[nodiscard]] std::string queryKey() const;
        [[nodiscard]] std::string quickFilterWindowKey() const;
        [[nodiscard]] javelin::jmap::search::EmailSearchCriteria filteredCriteria() const;
        [[nodiscard]] std::size_t nextOffset() const;

        std::string m_accountId;
        std::string m_mailboxId;
        QString m_title;
        std::optional<std::string> m_role;
        javelin::jmap::query::EmailListSort m_sort;
        javelin::jmap::search::EmailSearchCriteria m_quickFilter;
        std::string m_quickFilterSessionId;
        javelin::jmap::cache::QueryReader& m_queryReader;
        MessageListMaterializationPort& m_materializationPort;
        MailApplicationEventsPort& m_events;
        std::size_t m_windowSize;
        MessageListState m_state;
        std::vector<MessageListWindow> m_windows;
        MailboxObservationLease m_observation;
        std::optional<std::size_t> m_pendingLoadMoreOffset;
        bool m_pendingLoadMoreRequestCompleted = false;
        std::uint64_t m_generation = 0;
        std::uint64_t m_refreshRequestId = 0;
        std::uint64_t m_itemsRevision = 0;
        std::uint64_t m_cacheEpoch = 0;
        RefreshGeneration m_refreshGeneration;
        bool m_projectedReloadInFlight = false;
        bool m_projectedReloadPending = false;
        bool m_refreshAwaitingCache = false;
        bool m_endReached = false;
    };
} // namespace javelin::app
