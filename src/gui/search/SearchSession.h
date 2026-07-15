#pragma once

#include "app/LongPollCoordinator.h"
#include "jmap/cache/QueryService.h"
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

namespace javelin::gui::search
{

    struct SearchPageState
    {
        std::size_t offset = 0;
        std::optional<std::size_t> total;
        std::vector<javelin::jmap::cache::MessageListItem> items;
        bool cacheLoaded = false;
        bool refreshInFlight = false;
        bool stale = false;
        QString refreshError;
    };

    struct RestoredSearchState
    {
        SearchPageState page;
        bool authoritativeResults = false;
    };

    class SearchSession final : public QObject
    {
        Q_OBJECT

      public:
        SearchSession(std::string accountId, javelin::jmap::search::EmailSearchCriteria criteria,
                      javelin::jmap::query::EmailListSort sort,
                      javelin::jmap::cache::QueryService& queryService,
                      javelin::app::MailApplicationService& mailService, std::size_t pageSize,
                      std::optional<RestoredSearchState> restored = std::nullopt,
                      QObject* parent = nullptr);

        [[nodiscard]] const std::string& accountId() const;
        [[nodiscard]] const std::string& query() const;
        [[nodiscard]] const javelin::jmap::search::EmailSearchCriteria& criteria() const;
        [[nodiscard]] QString title() const;
        [[nodiscard]] const SearchPageState& page() const;

        void loadCachedPage(bool forceReload = false);
        void refreshFromServer();
        void markStale();
        void setSort(javelin::jmap::query::EmailListSort sort);
        [[nodiscard]] bool goToPreviousPage();
        [[nodiscard]] bool goToNextPage();
        void setSelectedEmailId(std::optional<std::string> emailId);

      Q_SIGNALS:
        void pageChanged();
        void refreshFailed(javelin::jmap::LiveRefreshError error);

      private:
        void startLocalSearch();
        void applyCommittedServerPage();
        void resetForPageChange();

        std::string m_accountId;
        std::string m_query;
        javelin::jmap::search::EmailSearchCriteria m_criteria;
        javelin::jmap::query::EmailListSort m_sort;
        javelin::jmap::cache::QueryService& m_queryService;
        javelin::app::MailApplicationService& m_mailService;
        std::size_t m_pageSize;
        SearchPageState m_page;
        std::optional<std::string> m_selectedEmailId;
        std::unordered_set<std::string> m_retainedLocalEmailIds;
        bool m_localSearchInFlight = false;
        bool m_authoritativeResultsApplied = false;
        std::uint64_t m_generation = 0;
    };

} // namespace javelin::gui::search
