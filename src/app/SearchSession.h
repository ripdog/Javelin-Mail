#pragma once

#include "app/MessageListSession.h"
#include "app/MessageListSessionFactory.h"
#include "app/RefreshGeneration.h"
#include "jmap/cache/QueryReader.h"
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
                      javelin::jmap::query::EmailListSort sort,
                      javelin::jmap::cache::QueryReader& queryReader,
                      javelin::app::MailApplicationService& mailService,
                      MailApplicationEventsPort& events, std::size_t pageSize,
                      std::optional<RestoredSearchState> restored = std::nullopt,
                      QObject* parent = nullptr);

        [[nodiscard]] const std::string& accountId() const override;
        [[nodiscard]] const std::string& query() const;
        [[nodiscard]] const javelin::jmap::search::EmailSearchCriteria& criteria() const;
        [[nodiscard]] QString title() const override;
        [[nodiscard]] const MessageListPage& page() const override;
        [[nodiscard]] SearchMode mode() const;
        [[nodiscard]] bool canPromoteToOnline() const;
        [[nodiscard]] const std::string& sessionId() const;

        void loadCachedPage(bool forceReload = false) override;
        void refresh() override;
        void promoteToOnline();
        void close();
        void refreshAfterMutation();
        void markStale() override;
        void setSort(javelin::jmap::query::EmailListSort sort) override;
        [[nodiscard]] bool goToPage(std::size_t pageIndex) override;
        [[nodiscard]] bool goToPreviousPage() override;
        [[nodiscard]] bool goToNextPage() override;

      private:
        void startLocalSnapshot();
        void applyLocalPage();
        void requestOnlinePage();
        void reloadProjectedPage();
        void prefetchOnlinePages(std::size_t offset, std::size_t remainingRequests,
                                 std::uint64_t generation, std::string queryState);
        void applyCommittedServerPage();
        void resetForPageChange();
        [[nodiscard]] std::string onlineWindowKey() const;

        std::string m_accountId;
        std::string m_query;
        javelin::jmap::search::EmailSearchCriteria m_criteria;
        javelin::jmap::query::EmailListSort m_sort;
        javelin::jmap::cache::QueryReader& m_queryReader;
        javelin::app::MailApplicationService& m_mailService;
        MailApplicationEventsPort& m_events;
        std::size_t m_pageSize;
        MessageListPage m_page;
        SearchMode m_mode = SearchMode::Local;
        std::string m_sessionId;
        std::vector<javelin::jmap::cache::MessageListItem> m_localSnapshot;
        std::unordered_set<std::size_t> m_prefetchOffsets;
        bool m_localSnapshotLoaded = false;
        bool m_localSearchInFlight = false;
        bool m_refreshAfterCurrent = false;
        bool m_closed = false;
        std::uint64_t m_generation = 0;
        std::uint64_t m_cacheEpoch = 0;
        RefreshGeneration m_refreshGeneration;
        bool m_projectedReloadInFlight = false;
        bool m_projectedReloadPending = false;
    };

} // namespace javelin::app
