#include "gui/search/SearchSession.h"

#include "jmap/cache/SearchResultReconciler.h"

#include <QCoroTask>

#include <QFutureWatcher>
#include <QtConcurrentRun>

#include <variant>

namespace javelin::gui::search
{
    namespace
    {
        using CachedSearchResult = std::variant<std::vector<javelin::jmap::cache::MessageListItem>,
                                                javelin::jmap::cache::DatabaseError>;

        [[nodiscard]] CachedSearchResult
        runCachedSearch(const QString& databasePath, const std::string& accountId,
                        const std::string& text, const std::size_t offset, const std::size_t limit)
        {
            javelin::jmap::cache::ThreadConnectionFactory factory{
                {.connectionNamePrefix = QStringLiteral("javelin-quick-search"),
                 .databasePath = databasePath}};
            auto connectionResult = factory.openForCurrentThread("query");
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&connectionResult))
            {
                return *error;
            }

            auto connection =
                std::get<javelin::jmap::cache::DatabaseConnection>(std::move(connectionResult));
            javelin::jmap::cache::QueryService queryService{connection};
            return queryService.searchCachedMessageText(accountId, text, limit, offset);
        }
    } // namespace

    SearchSession::SearchSession(std::string accountId,
                                 javelin::jmap::search::EmailSearchCriteria criteria,
                                 javelin::jmap::query::EmailListSort sort,
                                 javelin::jmap::cache::QueryService& queryService,
                                 javelin::app::MailApplicationService& mailService,
                                 const std::size_t pageSize,
                                 std::optional<RestoredSearchState> restored, QObject* parent)
        : QObject(parent), m_accountId(std::move(accountId)),
          m_query(javelin::jmap::search::displayString(criteria)), m_criteria(std::move(criteria)),
          m_sort(sort), m_queryService(queryService), m_mailService(mailService),
          m_pageSize(pageSize)
    {
        if (restored.has_value())
        {
            m_page = std::move(restored->page);
            m_authoritativeResultsApplied = restored->authoritativeResults;
        }

        connect(&m_mailService, &javelin::app::MailApplicationService::cacheCommitted, this,
                [this](const javelin::app::MailCacheChange& change)
                {
                    if (change.accountId.toStdString() != m_accountId)
                    {
                        return;
                    }
                    const auto key = javelin::jmap::search::cacheKey(m_criteria, m_sort);
                    for (const auto& window : change.searchWindows)
                    {
                        if (window.offset == m_page.offset && window.queryKey.toStdString() == key)
                        {
                            applyCommittedServerPage();
                            return;
                        }
                    }
                });
    }

    const std::string& SearchSession::accountId() const
    {
        return m_accountId;
    }

    const std::string& SearchSession::query() const
    {
        return m_query;
    }

    const javelin::jmap::search::EmailSearchCriteria& SearchSession::criteria() const
    {
        return m_criteria;
    }

    QString SearchSession::title() const
    {
        return QStringLiteral("Search: %1").arg(QString::fromStdString(m_query));
    }

    const SearchPageState& SearchSession::page() const
    {
        return m_page;
    }

    void SearchSession::loadCachedPage(const bool forceReload)
    {
        if (m_page.cacheLoaded && !forceReload)
        {
            return;
        }

        const auto key = javelin::jmap::search::cacheKey(m_criteria, m_sort);
        const auto result =
            m_queryService.loadSearchWindow(m_accountId, key, m_page.offset, m_pageSize);
        const auto* page =
            std::get_if<std::optional<javelin::jmap::cache::SearchWindowPage>>(&result);
        if (page == nullptr || !page->has_value())
        {
            m_page.items.clear();
            m_page.total.reset();
            m_page.cacheLoaded = page != nullptr;
            m_authoritativeResultsApplied = false;
            return;
        }

        m_page.items = (*page)->items;
        m_page.total = (*page)->total;
        m_page.cacheLoaded = true;
        m_authoritativeResultsApplied = true;
    }

    void SearchSession::refreshFromServer()
    {
        if (m_page.refreshInFlight)
        {
            return;
        }

        startLocalSearch();
        m_page.refreshInFlight = true;
        m_page.refreshError.clear();
        Q_EMIT pageChanged();
        const auto requestedOffset = m_page.offset;
        const auto generation = m_generation;
        auto task = m_mailService.requestSearchWindow(javelin::app::SearchWindowIntent{
            .accountId = m_accountId,
            .criteria = m_criteria,
            .offset = requestedOffset,
            .limit = m_pageSize,
            .sort = m_sort,
        });
        QCoro::connect(
            std::move(task), this,
            [this, requestedOffset, generation](javelin::app::SearchWindowResult result)
            {
                if (requestedOffset != m_page.offset || generation != m_generation)
                {
                    return;
                }
                if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
                {
                    m_page.refreshInFlight = false;
                    m_page.refreshError = error->message;
                    Q_EMIT pageChanged();
                    Q_EMIT refreshFailed(*error);
                    return;
                }

                const auto& summary = std::get<javelin::app::SearchWindowSummary>(result);
                m_page.total = summary.total;
                m_page.refreshInFlight = false;
                m_page.stale = false;
                m_page.refreshError.clear();
                Q_EMIT pageChanged();
            });
    }

    void SearchSession::markStale()
    {
        m_page.stale = true;
    }

    void SearchSession::setSort(javelin::jmap::query::EmailListSort sort)
    {
        if (m_sort.property == sort.property && m_sort.direction == sort.direction)
        {
            return;
        }
        m_sort = sort;
        m_page.offset = 0;
        m_page.total.reset();
        m_page.stale = true;
        resetForPageChange();
    }

    bool SearchSession::goToPreviousPage()
    {
        if (m_page.offset < m_pageSize)
        {
            return false;
        }
        m_page.offset -= m_pageSize;
        resetForPageChange();
        return true;
    }

    bool SearchSession::goToNextPage()
    {
        if (m_page.total.has_value() && m_page.offset + m_pageSize >= *m_page.total)
        {
            return false;
        }
        m_page.offset += m_pageSize;
        resetForPageChange();
        return true;
    }

    void SearchSession::setSelectedEmailId(std::optional<std::string> emailId)
    {
        m_selectedEmailId = std::move(emailId);
        if (m_retainedLocalEmailIds.empty())
        {
            return;
        }

        const auto previousSize = m_page.items.size();
        std::erase_if(m_page.items,
                      [this](const auto& item)
                      {
                          return m_retainedLocalEmailIds.contains(item.emailId) &&
                                 m_selectedEmailId != std::optional<std::string>{item.emailId};
                      });
        std::erase_if(m_retainedLocalEmailIds, [this](const auto& retainedId)
                      { return m_selectedEmailId != std::optional<std::string>{retainedId}; });
        if (m_page.items.size() != previousSize)
        {
            Q_EMIT pageChanged();
        }
    }

    void SearchSession::startLocalSearch()
    {
        if (!javelin::jmap::search::isBasicTextSearch(m_criteria) || m_localSearchInFlight ||
            m_authoritativeResultsApplied || !m_criteria.text.has_value())
        {
            return;
        }

        m_localSearchInFlight = true;
        const auto requestedOffset = m_page.offset;
        const auto generation = m_generation;
        auto* watcher = new QFutureWatcher<CachedSearchResult>(this);
        connect(watcher, &QFutureWatcher<CachedSearchResult>::finished, this,
                [this, watcher, requestedOffset, generation]
                {
                    const auto result = watcher->result();
                    watcher->deleteLater();
                    if (requestedOffset != m_page.offset || generation != m_generation)
                    {
                        return;
                    }
                    m_localSearchInFlight = false;
                    if (m_authoritativeResultsApplied ||
                        std::holds_alternative<javelin::jmap::cache::DatabaseError>(result))
                    {
                        return;
                    }
                    m_page.items =
                        std::get<std::vector<javelin::jmap::cache::MessageListItem>>(result);
                    m_page.cacheLoaded = true;
                    Q_EMIT pageChanged();
                });
        watcher->setFuture(QtConcurrent::run(runCachedSearch, m_queryService.databasePath(),
                                             m_accountId, *m_criteria.text, requestedOffset,
                                             m_pageSize));
    }

    void SearchSession::applyCommittedServerPage()
    {
        const auto priorItems = m_page.items;
        loadCachedPage(true);
        if (m_authoritativeResultsApplied && javelin::jmap::search::isBasicTextSearch(m_criteria))
        {
            const auto protectedId = m_selectedEmailId.has_value()
                                         ? std::optional<std::string_view>{*m_selectedEmailId}
                                         : std::nullopt;
            auto reconciled = javelin::jmap::cache::reconcileServerSearchResults(
                priorItems, m_page.items, protectedId);
            m_page.items = std::move(reconciled.items);
            m_retainedLocalEmailIds = std::move(reconciled.retainedLocalEmailIds);
        }
        m_page.stale = false;
        Q_EMIT pageChanged();
    }

    void SearchSession::resetForPageChange()
    {
        ++m_generation;
        m_page.items.clear();
        m_page.cacheLoaded = false;
        m_page.refreshInFlight = false;
        m_page.refreshError.clear();
        m_selectedEmailId.reset();
        m_retainedLocalEmailIds.clear();
        m_localSearchInFlight = false;
        m_authoritativeResultsApplied = false;
    }

} // namespace javelin::gui::search
