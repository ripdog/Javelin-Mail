#include "gui/search/SearchSession.h"

#include "gui/messages/Pagination.h"

#include <QCoroTask>

#include <QFutureWatcher>
#include <QtConcurrentRun>

#include <utility>
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
        if (m_page.cacheLoaded && !m_page.stale && !forceReload)
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
            // A missing search window is not an authoritative empty result. Keep rendering the
            // current identity set until a replacement server query commits.
            m_page.cacheLoaded = false;
            m_page.stale = true;
            return;
        }

        m_page.items = (*page)->items;
        m_page.position = (*page)->position;
        m_page.returnedLimit = (*page)->returnedLimit;
        m_page.total = (*page)->total;
        m_page.queryState = (*page)->queryState;
        m_page.cacheLoaded = (*page)->isAuthoritative;
        m_page.stale = !(*page)->isAuthoritative;
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
            .anchor = m_page.anchor,
        });
        QCoro::connect(
            std::move(task), this,
            [this, requestedOffset, generation](javelin::app::SearchWindowResult result)
            {
                if (requestedOffset != m_page.offset || generation != m_generation)
                {
                    return;
                }
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    m_page.refreshInFlight = false;
                    m_page.refreshError = error->message;
                    const bool refreshAgain = std::exchange(m_refreshAfterCurrent, false);
                    Q_EMIT pageChanged();
                    Q_EMIT refreshFailed(*error);
                    if (refreshAgain)
                    {
                        refreshFromServer();
                    }
                    return;
                }

                const auto& summary = std::get<javelin::app::SearchWindowSummary>(result);
                m_page.total = summary.total;
                m_page.position = summary.position;
                m_page.returnedLimit = summary.returnedLimit;
                m_page.queryState = summary.queryState;
                m_page.refreshInFlight = false;
                if (m_page.total.has_value() && m_page.offset > 0 &&
                    (*m_page.total == 0 || m_page.position >= *m_page.total))
                {
                    const auto step = m_page.returnedLimit == 0 ? m_pageSize : m_page.returnedLimit;
                    m_page.offset = javelin::gui::messages::normalizedPageOffset(
                        m_page.offset, *m_page.total, step);
                    m_page.anchor.reset();
                    resetForPageChange();
                    m_page.stale = true;
                    refreshFromServer();
                    return;
                }
                m_page.stale = false;
                m_page.refreshError.clear();
                const bool refreshAgain = std::exchange(m_refreshAfterCurrent, false);
                Q_EMIT pageChanged();
                if (refreshAgain)
                {
                    refreshFromServer();
                }
            });
    }

    void SearchSession::refreshAfterMutation()
    {
        if (m_page.refreshInFlight)
        {
            m_refreshAfterCurrent = true;
            return;
        }
        refreshFromServer();
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
        m_page.anchor.reset();
        m_page.total.reset();
        m_page.stale = true;
        resetForPageChange();
    }

    bool SearchSession::goToPreviousPage()
    {
        if (m_page.offset == 0)
        {
            return false;
        }
        const auto step = m_page.returnedLimit == 0 ? m_pageSize : m_page.returnedLimit;
        m_page.offset -= std::min(m_page.offset, step);
        m_page.anchor.reset();
        resetForPageChange();
        return true;
    }

    bool SearchSession::goToPage(const std::size_t pageIndex)
    {
        const auto step = m_page.returnedLimit == 0 ? m_pageSize : m_page.returnedLimit;
        if (m_page.total.has_value() &&
            pageIndex >= javelin::gui::messages::pageCount(*m_page.total, step))
        {
            return false;
        }
        const auto offset = javelin::gui::messages::pageOffset(pageIndex, step);
        if (offset == m_page.offset)
        {
            return false;
        }
        m_page.offset = offset;
        m_page.anchor.reset();
        resetForPageChange();
        return true;
    }

    bool SearchSession::goToNextPage()
    {
        if (m_page.total.has_value() && m_page.position + m_page.items.size() >= *m_page.total)
        {
            return false;
        }
        if (m_page.items.empty())
            return false;
        m_page.anchor = m_page.items.back().emailId;
        m_page.offset = m_page.position + m_page.items.size();
        resetForPageChange();
        return true;
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
        loadCachedPage(true);
        m_page.stale = false;
        Q_EMIT pageChanged();
    }

    void SearchSession::resetForPageChange()
    {
        ++m_generation;
        m_page.position = m_page.offset;
        m_page.items.clear();
        m_page.cacheLoaded = false;
        m_page.refreshInFlight = false;
        m_page.refreshError.clear();
        m_localSearchInFlight = false;
        m_authoritativeResultsApplied = false;
        m_refreshAfterCurrent = false;
    }

} // namespace javelin::gui::search
