#include "app/SearchSession.h"

#include "app/MailApplicationEventsPorts.h"
#include "app/MailApplicationService.h"
#include "jmap/cache/QueryService.h"
#include <QCoroTask>

#include <KLocalizedString>

#include <QFutureWatcher>
#include <QUuid>
#include <QtConcurrentRun>

#include <algorithm>
#include <utility>
#include <variant>

namespace javelin::app
{
    namespace
    {
        using LocalSearchResult = std::variant<std::vector<javelin::jmap::cache::MessageListItem>,
                                               javelin::jmap::cache::DatabaseError>;
        using ProjectedSearchPageResult =
            std::variant<std::optional<javelin::jmap::cache::SearchWindowPage>,
                         javelin::jmap::cache::DatabaseError>;

        constexpr std::size_t completeManifestThreshold = 2000;
        constexpr std::size_t readAheadPages = 2;

        [[nodiscard]] LocalSearchResult
        runLocalSearch(const QString& databasePath, const std::string& accountId,
                       const std::string& text, const javelin::jmap::query::EmailListSort sort)
        {
            javelin::jmap::cache::ReadOnlyThreadConnectionFactory factory{
                {.connectionNamePrefix = QStringLiteral("javelin-local-search"),
                 .databasePath = databasePath}};
            auto connectionResult = factory.openForCurrentThread("snapshot");
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&connectionResult))
            {
                return *error;
            }

            auto connection = std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(
                std::move(connectionResult));
            javelin::jmap::cache::QueryService queryService{connection};
            return queryService.searchAllCachedMessageText(accountId, text, sort);
        }

        [[nodiscard]] ProjectedSearchPageResult
        loadProjectedSearchPage(const QString& databasePath, const std::string& accountId,
                                const std::string& queryKey, const std::size_t offset,
                                const std::size_t limit)
        {
            javelin::jmap::cache::ReadOnlyThreadConnectionFactory factory{
                {.connectionNamePrefix = QStringLiteral("javelin-projected-search"),
                 .databasePath = databasePath}};
            auto connectionResult = factory.openForCurrentThread("snapshot");
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&connectionResult))
            {
                return *error;
            }
            auto connection = std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(
                std::move(connectionResult));
            javelin::jmap::cache::QueryService queryService{connection};
            return queryService.loadSearchWindow(accountId, queryKey, offset, limit);
        }
    } // namespace

    SearchSession::SearchSession(std::string accountId,
                                 javelin::jmap::search::EmailSearchCriteria criteria,
                                 javelin::jmap::query::EmailListSort sort,
                                 javelin::jmap::cache::QueryReader& queryReader,
                                 MessageListMaterializationPort& materializationPort,
                                 MailApplicationEventsPort& events, const std::size_t pageSize,
                                 std::optional<RestoredSearchState> restored, QObject* parent)
        : MessageListSession(parent), m_accountId(std::move(accountId)),
          m_query(javelin::jmap::search::displayString(criteria)), m_criteria(std::move(criteria)),
          m_sort(sort), m_queryReader(queryReader), m_materializationPort(materializationPort),
          m_events(events), m_pageSize(pageSize),
          m_mode(javelin::jmap::search::isBasicTextSearch(m_criteria) ? SearchMode::Local
                                                                      : SearchMode::Online),
          m_sessionId(QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString())
    {
        if (restored.has_value())
        {
            m_page = std::move(restored->page);
            m_mode = restored->mode == SearchMode::Promoting ? SearchMode::Online : restored->mode;
            if (!restored->sessionId.empty())
                m_sessionId = std::move(restored->sessionId);
        }

        connect(&m_events, &MailApplicationEventsPort::cacheInvalidated, this,
                [this](const MailCacheInvalidation& invalidation)
                {
                    const auto& change = invalidation.change;
                    if (m_closed || m_mode != SearchMode::Online ||
                        change.accountId.toStdString() != m_accountId)
                    {
                        return;
                    }
                    m_cacheEpoch = std::max(m_cacheEpoch, invalidation.epoch);
                    static_cast<void>(m_refreshGeneration.begin(m_cacheEpoch));
                    const auto key = onlineWindowKey();
                    for (const auto& window : change.searchWindows)
                    {
                        if (window.offset == m_page.offset && window.queryKey.toStdString() == key)
                        {
                            if (m_page.refreshInFlight)
                            {
                                ++m_refreshRequestId;
                                m_visiblePrefetchOffset.reset();
                                m_page.refreshInFlight = false;
                                Q_EMIT pageChanged();
                            }
                            applyCommittedServerPage();
                            return;
                        }
                    }
                    if (change.hasNewMail || change.optimisticProjection ||
                        !change.mailboxIds.isEmpty())
                    {
                        m_page.stale = true;
                        applyCommittedServerPage();
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
        return i18nc("@title search tab", "Search: %1", QString::fromStdString(m_query));
    }

    const MessageListPage& SearchSession::page() const
    {
        return m_page;
    }

    SearchMode SearchSession::mode() const
    {
        return m_mode;
    }

    bool SearchSession::canPromoteToOnline() const
    {
        return m_mode == SearchMode::Local;
    }

    const std::string& SearchSession::sessionId() const
    {
        return m_sessionId;
    }

    void SearchSession::loadCachedPage(const bool forceReload)
    {
        if (m_mode == SearchMode::Local)
        {
            if (forceReload)
                m_localSnapshotLoaded = false;
            if (!m_localSnapshotLoaded)
                startLocalSnapshot();
            else
                applyLocalPage();
            return;
        }
        if (m_mode == SearchMode::Promoting)
            return;
        if (m_page.cacheLoaded && !m_page.stale && !forceReload)
            return;

        reloadProjectedPage();
    }

    void SearchSession::refresh()
    {
        if (m_mode == SearchMode::Local)
        {
            if (!m_localSnapshotLoaded)
                startLocalSnapshot();
            return;
        }
        if (m_mode == SearchMode::Promoting)
            return;
        requestOnlinePage();
    }

    void SearchSession::promoteToOnline()
    {
        if (m_mode != SearchMode::Local)
            return;

        ++m_generation;
        ++m_refreshRequestId;
        m_visiblePrefetchOffset.reset();
        m_refreshGeneration.replaceScope();
        m_mode = SearchMode::Promoting;
        m_localSearchInFlight = false;
        m_page = MessageListPage{
            .offset = 0,
            .installedOffset = std::nullopt,
            .pendingOffset = std::nullopt,
            .position = 0,
            .returnedLimit = m_pageSize,
            .total = std::nullopt,
            .queryState = {},
            .anchor = std::nullopt,
            .items = {},
            .cacheLoaded = false,
            .refreshInFlight = false,
            .stale = true,
            .refreshError = {},
        };
        Q_EMIT pageChanged();
        m_mode = SearchMode::Online;
        requestOnlinePage();
    }

    void SearchSession::requestOnlinePage()
    {
        if (m_page.refreshInFlight)
            return;
        if (m_prefetchOffsets.contains(m_page.offset))
        {
            ++m_refreshRequestId;
            m_visiblePrefetchOffset = m_page.offset;
            m_page.refreshInFlight = true;
            Q_EMIT pageChanged();
            return;
        }

        const auto requestId = ++m_refreshRequestId;
        m_visiblePrefetchOffset.reset();
        m_page.refreshInFlight = true;
        m_page.refreshError.clear();
        Q_EMIT pageChanged();
        const auto requestedOffset = m_page.offset;
        const auto generation = m_generation;
        auto task = m_materializationPort.requestSearchWindow(SearchWindowIntent{
            .accountId = m_accountId,
            .criteria = m_criteria,
            .offset = requestedOffset,
            .limit = m_pageSize,
            .sort = m_sort,
            .anchor = m_page.anchor,
            .windowKey = onlineWindowKey(),
        });
        QCoro::connect(
            std::move(task), this,
            [this, requestedOffset, generation, requestId](SearchWindowResult result)
            {
                if (requestId != m_refreshRequestId || m_closed ||
                    requestedOffset != m_page.offset || generation != m_generation ||
                    m_mode != SearchMode::Online)
                {
                    return;
                }
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    m_page.refreshInFlight = false;
                    m_page.refreshError = error->message;
                    const bool wasInitialPromotion = m_page.queryState.empty();
                    if (wasInitialPromotion && javelin::jmap::search::isBasicTextSearch(m_criteria))
                    {
                        m_mode = SearchMode::Local;
                        m_page.refreshError.clear();
                        if (m_localSnapshotLoaded)
                            applyLocalPage();
                        else
                            startLocalSnapshot();
                    }
                    const bool refreshAgain = std::exchange(m_refreshAfterCurrent, false);
                    Q_EMIT pageChanged();
                    Q_EMIT refreshFailed(*error);
                    if (refreshAgain && m_mode == SearchMode::Online)
                        requestOnlinePage();
                    return;
                }

                const auto& summary = std::get<SearchWindowSummary>(result);
                m_localSnapshot.clear();
                m_localSnapshotLoaded = false;
                m_page.total = summary.total;
                m_page.position = summary.position;
                m_page.returnedLimit = summary.returnedLimit;
                m_page.queryState = summary.queryState;
                m_page.refreshInFlight = false;
                m_page.anchor.reset();
                if (m_page.total.has_value() && m_page.offset > 0 &&
                    (*m_page.total == 0 || m_page.position >= *m_page.total))
                {
                    const auto step = m_page.returnedLimit == 0 ? m_pageSize : m_page.returnedLimit;
                    m_page.offset =
                        normalizedMessageListPageOffset(m_page.offset, *m_page.total, step);
                    resetForPageChange();
                    requestOnlinePage();
                    return;
                }
                applyCommittedServerPage();
                m_page.stale = false;
                m_page.refreshError.clear();
                const auto nextOffset = summary.position + summary.representativeCount;
                const auto remainingRequests =
                    summary.total.has_value() && *summary.total <= completeManifestThreshold
                        ? (*summary.total > nextOffset ? *summary.total - nextOffset : 0)
                        : readAheadPages;
                if (summary.representativeCount != 0 && remainingRequests > 0)
                {
                    prefetchOnlinePages(nextOffset, remainingRequests, generation,
                                        summary.queryState);
                }
                const bool refreshAgain = std::exchange(m_refreshAfterCurrent, false);
                Q_EMIT pageChanged();
                if (refreshAgain)
                    requestOnlinePage();
            });
    }

    void SearchSession::prefetchOnlinePages(const std::size_t offset,
                                            const std::size_t remainingRequests,
                                            const std::uint64_t generation, std::string queryState)
    {
        if (remainingRequests == 0 || generation != m_generation || m_mode != SearchMode::Online ||
            (m_page.total.has_value() && offset >= *m_page.total))
        {
            return;
        }

        m_prefetchOffsets.insert(offset);
        auto* watcher = new QFutureWatcher<ProjectedSearchPageResult>(this);
        connect(
            watcher, &QFutureWatcher<ProjectedSearchPageResult>::finished, this,
            [this, watcher, offset, remainingRequests, generation,
             queryState = std::move(queryState)]
            {
                auto cached = watcher->result();
                watcher->deleteLater();
                if (m_closed || generation != m_generation || m_mode != SearchMode::Online)
                {
                    m_prefetchOffsets.erase(offset);
                    return;
                }

                if (const auto* page =
                        std::get_if<std::optional<javelin::jmap::cache::SearchWindowPage>>(&cached);
                    page != nullptr && page->has_value() &&
                    javelin::jmap::cache::isPaginationAuthoritative((*page)->coverage,
                                                                    (*page)->materialization) &&
                    (*page)->queryState == queryState)
                {
                    m_prefetchOffsets.erase(offset);
                    if (m_page.offset == offset && m_visiblePrefetchOffset == offset)
                    {
                        m_visiblePrefetchOffset.reset();
                        m_page.refreshInFlight = false;
                        applyCommittedServerPage();
                        Q_EMIT pageChanged();
                    }
                    const auto next = (*page)->position + (*page)->items.size();
                    if (next > offset)
                        prefetchOnlinePages(next, remainingRequests - 1, generation,
                                            std::move(queryState));
                    return;
                }

                auto task = m_materializationPort.requestSearchWindow(SearchWindowIntent{
                    .accountId = m_accountId,
                    .criteria = m_criteria,
                    .offset = offset,
                    .limit = m_pageSize,
                    .sort = m_sort,
                    .anchor = std::nullopt,
                    .windowKey = onlineWindowKey(),
                });
                QCoro::connect(
                    std::move(task), this,
                    [this, offset, remainingRequests, generation,
                     queryState = std::move(queryState)](SearchWindowResult result)
                    {
                        m_prefetchOffsets.erase(offset);
                        if (m_closed || generation != m_generation || m_mode != SearchMode::Online)
                            return;
                        const auto* summary = std::get_if<SearchWindowSummary>(&result);
                        const bool visibleCurrentPage =
                            m_page.offset == offset && m_visiblePrefetchOffset == offset;
                        if (summary == nullptr || summary->queryState != queryState)
                        {
                            if (visibleCurrentPage)
                            {
                                m_visiblePrefetchOffset.reset();
                                m_page.refreshInFlight = false;
                                m_page.stale = true;
                                Q_EMIT pageChanged();
                                requestOnlinePage();
                            }
                            else if (summary != nullptr)
                            {
                                m_page.stale = true;
                                Q_EMIT pageChanged();
                            }
                            return;
                        }
                        if (visibleCurrentPage)
                        {
                            m_visiblePrefetchOffset.reset();
                            m_page.refreshInFlight = false;
                            applyCommittedServerPage();
                            Q_EMIT pageChanged();
                        }
                        if (summary->representativeCount == 0)
                            return;
                        const auto next = summary->position + summary->representativeCount;
                        if (next > offset)
                        {
                            prefetchOnlinePages(next, remainingRequests - 1, generation,
                                                std::move(queryState));
                        }
                    });
            });
        watcher->setFuture(QtConcurrent::run(loadProjectedSearchPage, m_queryReader.databasePath(),
                                             m_accountId, onlineWindowKey(), offset, m_pageSize));
    }

    void SearchSession::refreshAfterMutation()
    {
        if (m_mode != SearchMode::Online)
            return;
        if (m_page.refreshInFlight)
        {
            m_refreshAfterCurrent = true;
            return;
        }
        requestOnlinePage();
    }

    void SearchSession::close()
    {
        m_closed = true;
        ++m_generation;
        ++m_refreshRequestId;
        m_visiblePrefetchOffset.reset();
        m_refreshGeneration.close();
        if (m_mode == SearchMode::Online)
            m_materializationPort.retireSearchWindow(m_accountId, onlineWindowKey());
    }

    void SearchSession::markStale()
    {
        if (m_mode == SearchMode::Online)
            m_page.stale = true;
    }

    void SearchSession::setSort(javelin::jmap::query::EmailListSort sort)
    {
        if (m_sort.property == sort.property && m_sort.direction == sort.direction)
            return;
        if (m_mode == SearchMode::Online)
        {
            m_materializationPort.retireSearchWindow(m_accountId, onlineWindowKey());
            m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        }
        m_sort = sort;
        ++m_generation;
        ++m_refreshRequestId;
        m_visiblePrefetchOffset.reset();
        m_refreshGeneration.replaceScope();
        m_page.offset = 0;
        m_page.position = 0;
        m_page.anchor.reset();
        m_page.total.reset();
        m_page.items.clear();
        m_page.cacheLoaded = false;
        m_page.stale = true;
        m_page.refreshInFlight = false;
        m_localSnapshot.clear();
        m_localSnapshotLoaded = false;
        m_prefetchOffsets.clear();
    }

    bool SearchSession::goToPreviousPage()
    {
        if (m_page.offset == 0)
            return false;
        const auto step = m_page.returnedLimit == 0 ? m_pageSize : m_page.returnedLimit;
        m_page.offset -= std::min(m_page.offset, step);
        m_page.anchor.reset();
        resetForPageChange();
        return true;
    }

    bool SearchSession::goToPage(const std::size_t pageIndex)
    {
        const auto step = m_page.returnedLimit == 0 ? m_pageSize : m_page.returnedLimit;
        if (m_page.total.has_value() && pageIndex >= messageListPageCount(*m_page.total, step))
        {
            return false;
        }
        const auto offset = messageListPageOffset(pageIndex, step);
        if (offset == m_page.offset)
            return false;
        m_page.offset = offset;
        m_page.anchor.reset();
        resetForPageChange();
        return true;
    }

    bool SearchSession::goToNextPage()
    {
        if (m_page.total.has_value() && m_page.position + m_page.items.size() >= *m_page.total)
            return false;
        if (m_page.items.empty())
            return false;
        if (m_mode == SearchMode::Online)
            m_page.anchor = m_page.items.back().emailId;
        m_page.offset = m_page.position + m_page.items.size();
        resetForPageChange();
        return true;
    }

    void SearchSession::startLocalSnapshot()
    {
        if (m_mode != SearchMode::Local || m_localSearchInFlight ||
            !javelin::jmap::search::isBasicTextSearch(m_criteria) || !m_criteria.text.has_value())
        {
            return;
        }

        m_localSearchInFlight = true;
        const auto generation = m_generation;
        const auto ticket = m_refreshGeneration.begin(m_cacheEpoch);
        auto* watcher = new QFutureWatcher<LocalSearchResult>(this);
        connect(
            watcher, &QFutureWatcher<LocalSearchResult>::finished, this,
            [this, watcher, generation, ticket]
            {
                auto result = watcher->result();
                watcher->deleteLater();
                m_localSearchInFlight = false;
                if (generation != m_generation || m_mode != SearchMode::Local ||
                    !m_refreshGeneration.install(ticket, m_cacheEpoch))
                {
                    if (!m_closed && m_mode == SearchMode::Local && !m_localSnapshotLoaded)
                        startLocalSnapshot();
                    return;
                }
                if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                {
                    m_page.refreshError = error->message;
                    Q_EMIT pageChanged();
                    return;
                }
                m_localSnapshot =
                    std::get<std::vector<javelin::jmap::cache::MessageListItem>>(std::move(result));
                m_localSnapshotLoaded = true;
                m_page.total = m_localSnapshot.size();
                m_page.queryState.clear();
                m_page.stale = false;
                applyLocalPage();
                Q_EMIT pageChanged();
            });
        watcher->setFuture(QtConcurrent::run(runLocalSearch, m_queryReader.databasePath(),
                                             m_accountId, *m_criteria.text, m_sort));
    }

    void SearchSession::applyLocalPage()
    {
        if (m_mode != SearchMode::Local)
            return;
        const auto begin = std::min(m_page.offset, m_localSnapshot.size());
        const auto end = std::min(begin + m_pageSize, m_localSnapshot.size());
        m_page.items.assign(m_localSnapshot.begin() + static_cast<std::ptrdiff_t>(begin),
                            m_localSnapshot.begin() + static_cast<std::ptrdiff_t>(end));
        m_page.position = begin;
        m_page.returnedLimit = m_pageSize;
        m_page.total = m_localSnapshot.size();
        m_page.installedOffset = m_page.offset;
        m_page.pendingOffset.reset();
        m_page.cacheLoaded = true;
        m_page.refreshInFlight = false;
        m_page.stale = false;
    }

    void SearchSession::applyCommittedServerPage()
    {
        loadCachedPage(true);
    }

    void SearchSession::reloadProjectedPage()
    {
        if (m_projectedReloadInFlight)
        {
            m_projectedReloadPending = true;
            return;
        }
        m_projectedReloadInFlight = true;
        const auto generation = m_generation;
        const auto offset = m_page.offset;
        const auto queryKey = onlineWindowKey();
        const auto ticket = m_refreshGeneration.begin(m_cacheEpoch);
        auto* watcher = new QFutureWatcher<ProjectedSearchPageResult>(this);
        connect(watcher, &QFutureWatcher<ProjectedSearchPageResult>::finished, this,
                [this, watcher, generation, offset, ticket]
                {
                    auto result = watcher->result();
                    watcher->deleteLater();
                    m_projectedReloadInFlight = false;
                    if (m_closed || m_mode != SearchMode::Online || generation != m_generation ||
                        offset != m_page.offset ||
                        !m_refreshGeneration.install(ticket, m_cacheEpoch))
                    {
                        if (std::exchange(m_projectedReloadPending, false))
                            reloadProjectedPage();
                        return;
                    }

                    const auto* page =
                        std::get_if<std::optional<javelin::jmap::cache::SearchWindowPage>>(&result);
                    if (page == nullptr || !page->has_value() ||
                        !javelin::jmap::cache::isDisplayCurrent((*page)->coverage,
                                                                (*page)->materialization) ||
                        (!m_page.queryState.empty() && (*page)->queryState != m_page.queryState))
                    {
                        m_page.cacheLoaded = false;
                        m_page.stale = true;
                    }
                    else
                    {
                        m_page.items = (*page)->items;
                        m_page.position = (*page)->position;
                        m_page.returnedLimit = (*page)->returnedLimit;
                        m_page.total = (*page)->total;
                        m_page.queryState = (*page)->queryState;
                        m_page.cacheLoaded = true;
                        m_page.stale = false;
                        m_page.installedOffset = m_page.offset;
                        m_page.pendingOffset.reset();
                    }
                    Q_EMIT pageChanged();
                    if (std::exchange(m_projectedReloadPending, false))
                        reloadProjectedPage();
                });
        watcher->setFuture(QtConcurrent::run(loadProjectedSearchPage, m_queryReader.databasePath(),
                                             m_accountId, queryKey, offset, m_pageSize));
    }

    void SearchSession::resetForPageChange()
    {
        ++m_generation;
        ++m_refreshRequestId;
        m_visiblePrefetchOffset.reset();
        static_cast<void>(m_refreshGeneration.begin(m_cacheEpoch));
        m_page.pendingOffset = m_page.offset;
        m_page.position = m_page.offset;
        m_page.items.clear();
        m_page.cacheLoaded = false;
        m_page.refreshInFlight = false;
        m_page.refreshError.clear();
        m_refreshAfterCurrent = false;
        if (m_mode == SearchMode::Local)
            applyLocalPage();
    }

    std::string SearchSession::onlineWindowKey() const
    {
        return javelin::jmap::search::cacheKey(m_criteria, m_sort) + "|session:" + m_sessionId;
    }
} // namespace javelin::app
