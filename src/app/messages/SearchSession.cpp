#include "app/SearchSession.h"

#include "app/MailApplicationEventsPorts.h"
#include "jmap/cache/MailSearchReadRepository.h"
#include "jmap/cache/MailboxMessageReadRepository.h"
#include "jmap/cache/QueryWindowReadRepository.h"

#include <QCoroTask>

#include <KLocalizedString>

#include <QFutureWatcher>
#include <QUuid>
#include <QtConcurrentRun>

#include <algorithm>
#include <unordered_set>
#include <utility>
#include <variant>

namespace javelin::app
{
    namespace
    {
        using LocalSearchResult = std::variant<std::vector<javelin::jmap::cache::MessageListItem>,
                                               javelin::jmap::cache::DatabaseError>;

        struct WindowRequest
        {
            std::size_t offset = 0;
            std::size_t limit = 0;
        };

        using ProjectedSearchWindows =
            std::vector<std::optional<javelin::jmap::cache::SearchWindowPage>>;
        using ProjectedSearchWindowsResult =
            std::variant<ProjectedSearchWindows, javelin::jmap::cache::DatabaseError>;

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
            javelin::jmap::cache::MailSearchReadRepository mailSearch{connection};
            return mailSearch.searchAllCachedMessageText(accountId, text, sort);
        }

        [[nodiscard]] ProjectedSearchWindowsResult
        loadProjectedSearchWindows(const QString& databasePath, const std::string& accountId,
                                   const std::string& queryKey,
                                   const std::vector<WindowRequest>& requests)
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
            javelin::jmap::cache::MailboxMessageReadRepository mailboxMessages{connection};
            javelin::jmap::cache::QueryWindowReadRepository queryWindows{connection,
                                                                         mailboxMessages};
            ProjectedSearchWindows windows;
            windows.reserve(requests.size());
            for (const auto& request : requests)
            {
                auto result = queryWindows.loadSearchWindow(accountId, queryKey, request.offset,
                                                            request.limit);
                if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                {
                    return *error;
                }
                windows.push_back(std::get<std::optional<javelin::jmap::cache::SearchWindowPage>>(
                    std::move(result)));
            }
            return windows;
        }

        [[nodiscard]] bool displayCurrent(const javelin::jmap::cache::SearchWindowPage& page)
        {
            return javelin::jmap::cache::isDisplayCurrent(page.coverage, page.materialization);
        }
    } // namespace

    SearchSession::SearchSession(std::string accountId,
                                 javelin::jmap::search::EmailSearchCriteria criteria,
                                 javelin::jmap::query::EmailListSort sort, QString databasePath,
                                 MessageListMaterializationPort& materializationPort,
                                 MailApplicationEventsPort& events, const std::size_t windowSize,
                                 std::optional<RestoredSearchState> restored, QObject* parent)
        : MessageListSession(parent), m_accountId(std::move(accountId)),
          m_query(javelin::jmap::search::displayString(criteria)), m_criteria(std::move(criteria)),
          m_sort(sort), m_databasePath(std::move(databasePath)),
          m_materializationPort(materializationPort), m_events(events), m_windowSize(windowSize),
          m_mode(javelin::jmap::search::isBasicTextSearch(m_criteria) ? SearchMode::Local
                                                                      : SearchMode::Online),
          m_sessionId(QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString())
    {
        if (restored.has_value())
        {
            m_mode = restored->mode == SearchMode::Promoting ? SearchMode::Online : restored->mode;
            if (!restored->sessionId.empty())
                m_sessionId = std::move(restored->sessionId);
        }
        if (m_mode == SearchMode::Online)
        {
            if (restored.has_value() && !restored->windows.empty())
            {
                const auto count =
                    std::min(restored->windows.size(), maximumRestoredMessageListWindows);
                m_windows.reserve(count);
                for (std::size_t index = 0; index < count; ++index)
                {
                    const auto& request = restored->windows[index];
                    if (request.limit == 0)
                        break;
                    m_windows.push_back({
                        .requestedOffset = request.offset,
                        .requestedLimit = request.limit,
                        .position = 0,
                        .returnedLimit = 0,
                        .total = std::nullopt,
                        .queryState = {},
                        .itemCount = 0,
                        .displayCurrent = false,
                    });
                }
            }
            if (m_windows.empty())
                resetOnlineWindows();
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
                    const auto beginRelevantRefresh = [this]
                    { static_cast<void>(m_refreshGeneration.begin(m_cacheEpoch)); };
                    const auto key = onlineWindowKey();
                    bool retainedWindowChanged = false;
                    for (const auto& changed : change.searchWindows)
                    {
                        if (changed.queryKey.toStdString() != key)
                            continue;
                        retainedWindowChanged =
                            std::ranges::any_of(m_windows,
                                                [&changed](const MessageListWindow& window)
                                                {
                                                    return window.requestedOffset ==
                                                               changed.offset &&
                                                           window.requestedLimit == changed.limit;
                                                }) ||
                            retainedWindowChanged;
                    }
                    if (retainedWindowChanged)
                    {
                        beginRelevantRefresh();
                        if (m_state.refreshInFlight)
                        {
                            ++m_refreshRequestId;
                            m_state.refreshInFlight = false;
                            m_refreshAwaitingCache = false;
                            Q_EMIT stateChanged();
                        }
                        reloadProjectedWindows();
                        return;
                    }

                    if (change.optimisticProjection || !change.mailboxIds.isEmpty() ||
                        change.mailTagsChanged)
                    {
                        beginRelevantRefresh();
                        m_state.stale = true;
                        reloadProjectedWindows();
                    }
                });
        connect(&m_events, &MailApplicationEventsPort::threadMaterializationProgress, this,
                [this](const ThreadMaterializationProgress& progress)
                {
                    if (m_closed || m_mode != SearchMode::Online ||
                        progress.accountId.toStdString() != m_accountId)
                    {
                        return;
                    }
                    m_materializingThreadIds.clear();
                    if (progress.inFlight)
                    {
                        for (const auto& threadId : progress.threadIds)
                            m_materializingThreadIds.insert(threadId.toStdString());
                    }
                    if (updateThreadMaterializationState())
                        Q_EMIT stateChanged();
                });
        connect(&m_events, &MailApplicationEventsPort::accountStatusChanged, this,
                [this](const QString& changedAccountId, const MailAccountStatus status)
                {
                    if (m_closed || changedAccountId.toStdString() != m_accountId ||
                        (status != MailAccountStatus::Disconnected &&
                         status != MailAccountStatus::AuthenticationPaused))
                    {
                        return;
                    }
                    m_materializingThreadIds.clear();
                    if (updateThreadMaterializationState())
                        Q_EMIT stateChanged();
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

    const MessageListState& SearchSession::state() const
    {
        return m_state;
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

    void SearchSession::loadCachedState(const bool forceReload)
    {
        if (m_mode == SearchMode::Local)
        {
            if (forceReload)
                m_localSnapshotLoaded = false;
            if (!m_localSnapshotLoaded)
                startLocalSnapshot();
            else
                applyLocalVisibleRange();
            return;
        }
        if (m_mode == SearchMode::Promoting)
            return;
        if (m_state.cacheLoaded && !m_state.stale && !forceReload)
            return;
        reloadProjectedWindows();
    }

    void SearchSession::refresh(const MessageListRefreshMode)
    {
        if (m_mode == SearchMode::Local)
        {
            if (!m_localSnapshotLoaded)
                startLocalSnapshot();
            return;
        }
        if (m_mode == SearchMode::Online)
            requestOnlineInitial();
    }

    void SearchSession::promoteToOnline()
    {
        if (m_mode != SearchMode::Local)
            return;

        ++m_generation;
        ++m_refreshRequestId;
        m_refreshGeneration.replaceScope();
        m_mode = SearchMode::Promoting;
        m_localSearchInFlight = false;
        m_localSnapshot.clear();
        m_localSnapshotLoaded = false;
        m_localVisibleCount = 0;
        m_state = MessageListState{};
        m_state.itemsRevision = ++m_itemsRevision;
        m_prefetchOffsets.clear();
        m_pendingLoadMoreOffset.reset();
        m_pendingLoadMoreAnchor.reset();
        m_pendingLoadMoreCommitted = false;
        m_pendingLoadMoreRequestCompleted = false;
        resetOnlineWindows();
        Q_EMIT stateChanged();

        m_mode = SearchMode::Online;
        requestOnlineInitial();
    }

    void SearchSession::requestOnlineInitial()
    {
        if (m_closed || m_mode != SearchMode::Online || m_state.refreshInFlight)
            return;

        if (m_state.loadMoreInFlight)
        {
            ++m_refreshRequestId;
            m_state.loadMoreInFlight = false;
            m_pendingLoadMoreOffset.reset();
            m_pendingLoadMoreAnchor.reset();
            m_pendingLoadMoreCommitted = false;
            m_pendingLoadMoreRequestCompleted = false;
        }

        m_state.refreshInFlight = true;
        m_state.refreshError.clear();
        m_state.loadMoreError.clear();
        m_state.stale = true;
        Q_EMIT stateChanged();

        const auto generation = m_generation;
        const auto requestId = ++m_refreshRequestId;
        auto task = m_materializationPort.requestSearchWindow(SearchWindowIntent{
            .accountId = m_accountId,
            .criteria = m_criteria,
            .offset = 0,
            .limit = m_windowSize,
            .sort = m_sort,
            .anchor = std::nullopt,
            .windowKey = onlineWindowKey(),
        });
        QCoro::connect(
            std::move(task), this,
            [this, generation, requestId](SearchWindowResult result)
            {
                if (m_closed || m_mode != SearchMode::Online || generation != m_generation ||
                    requestId != m_refreshRequestId)
                {
                    return;
                }

                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    m_state.refreshInFlight = false;
                    m_state.refreshError = error->message;
                    const bool wasInitialPromotion = m_windows.size() == 1 &&
                                                     m_windows.front().queryState.empty() &&
                                                     m_state.items.empty();
                    if (wasInitialPromotion && javelin::jmap::search::isBasicTextSearch(m_criteria))
                    {
                        m_mode = SearchMode::Local;
                        m_state.refreshError.clear();
                        m_windows.clear();
                        if (m_localSnapshotLoaded)
                            applyLocalVisibleRange();
                        else
                            startLocalSnapshot();
                    }
                    const bool refreshAgain = std::exchange(m_refreshAfterCurrent, false);
                    Q_EMIT stateChanged();
                    Q_EMIT refreshFailed(*error);
                    if (refreshAgain && m_mode == SearchMode::Online)
                        requestOnlineInitial();
                    return;
                }

                const auto& summary = std::get<SearchWindowSummary>(result);
                m_windows.clear();
                m_windows.push_back({
                    .requestedOffset = summary.offset,
                    .requestedLimit = summary.limit,
                    .position = summary.position,
                    .returnedLimit = summary.returnedLimit,
                    .total = summary.total,
                    .queryState = summary.queryState,
                    .itemCount = summary.representativeCount,
                    .displayCurrent = false,
                });
                m_pendingLoadMoreOffset.reset();
                m_pendingLoadMoreAnchor.reset();
                m_pendingLoadMoreCommitted = false;
                m_pendingLoadMoreRequestCompleted = false;
                m_state.loadMoreInFlight = false;
                m_endReached = summary.representativeCount == 0 ||
                               (summary.total.has_value() &&
                                summary.position + summary.representativeCount >= *summary.total) ||
                               (!summary.total.has_value() && summary.returnedLimit > 0 &&
                                summary.representativeCount < summary.returnedLimit);
                m_refreshAwaitingCache = true;
                reloadProjectedWindows();
            });
    }

    void SearchSession::requestOnlineContinuation(const std::size_t offset, std::string anchor)
    {
        if (m_closed || m_mode != SearchMode::Online || !m_state.loadMoreInFlight ||
            !m_pendingLoadMoreOffset.has_value() || *m_pendingLoadMoreOffset != offset)
        {
            return;
        }

        if (std::ranges::none_of(m_windows, [offset](const MessageListWindow& window)
                                 { return window.requestedOffset == offset; }))
        {
            m_windows.push_back({
                .requestedOffset = offset,
                .requestedLimit = m_windowSize,
                .position = 0,
                .returnedLimit = 0,
                .total = std::nullopt,
                .queryState = {},
                .itemCount = 0,
                .displayCurrent = false,
            });
        }

        const auto generation = m_generation;
        const auto requestId = ++m_refreshRequestId;
        m_pendingLoadMoreCommitted = true;
        m_pendingLoadMoreRequestCompleted = false;
        auto task = m_materializationPort.requestSearchWindow(SearchWindowIntent{
            .accountId = m_accountId,
            .criteria = m_criteria,
            .offset = offset,
            .limit = m_windowSize,
            .sort = m_sort,
            .anchor = std::move(anchor),
            .windowKey = onlineWindowKey(),
        });
        QCoro::connect(
            std::move(task), this,
            [this, generation, requestId, offset](SearchWindowResult result)
            {
                if (m_closed || m_mode != SearchMode::Online || generation != m_generation ||
                    requestId != m_refreshRequestId || !m_pendingLoadMoreOffset.has_value() ||
                    *m_pendingLoadMoreOffset != offset)
                {
                    return;
                }

                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    const auto pending =
                        std::ranges::find_if(m_windows, [offset](const MessageListWindow& window)
                                             { return window.requestedOffset == offset; });
                    if (pending != m_windows.end())
                        m_windows.erase(pending);
                    m_state.loadMoreInFlight = false;
                    m_state.loadMoreError = error->message;
                    m_pendingLoadMoreOffset.reset();
                    m_pendingLoadMoreAnchor.reset();
                    m_pendingLoadMoreCommitted = false;
                    m_pendingLoadMoreRequestCompleted = false;
                    Q_EMIT stateChanged();
                    return;
                }

                const auto& summary = std::get<SearchWindowSummary>(result);
                const auto existing =
                    std::ranges::find_if(m_windows, [&summary](const MessageListWindow& window)
                                         { return window.requestedOffset == summary.offset; });
                if (existing == m_windows.end())
                {
                    m_windows.push_back({
                        .requestedOffset = summary.offset,
                        .requestedLimit = summary.limit,
                        .position = summary.position,
                        .returnedLimit = summary.returnedLimit,
                        .total = summary.total,
                        .queryState = summary.queryState,
                        .itemCount = summary.representativeCount,
                        .displayCurrent = false,
                    });
                }
                else
                {
                    existing->requestedLimit = summary.limit;
                    existing->position = summary.position;
                    existing->returnedLimit = summary.returnedLimit;
                    existing->total = summary.total;
                    existing->queryState = summary.queryState;
                    existing->itemCount = summary.representativeCount;
                    existing->displayCurrent = false;
                }
                m_pendingLoadMoreRequestCompleted = true;
                m_endReached = summary.representativeCount == 0 ||
                               (summary.total.has_value() &&
                                summary.position + summary.representativeCount >= *summary.total) ||
                               (!summary.total.has_value() && summary.returnedLimit > 0 &&
                                summary.representativeCount < summary.returnedLimit);
                reloadProjectedWindows();
            });
    }

    void SearchSession::reloadProjectedWindows()
    {
        if (m_closed || m_mode != SearchMode::Online)
            return;
        if (m_projectedReloadInFlight)
        {
            m_projectedReloadPending = true;
            return;
        }
        if (m_windows.empty())
            resetOnlineWindows();

        std::vector<WindowRequest> requests;
        requests.reserve(m_windows.size());
        for (const auto& window : m_windows)
        {
            requests.push_back({
                .offset = window.requestedOffset,
                .limit = window.requestedLimit,
            });
        }

        m_projectedReloadInFlight = true;
        const auto generation = m_generation;
        const auto ticket = m_refreshGeneration.begin(m_cacheEpoch);
        auto* watcher = new QFutureWatcher<ProjectedSearchWindowsResult>(this);
        connect(
            watcher, &QFutureWatcher<ProjectedSearchWindowsResult>::finished, this,
            [this, watcher, generation, ticket]
            {
                auto result = watcher->result();
                watcher->deleteLater();
                m_projectedReloadInFlight = false;

                if (m_closed || m_mode != SearchMode::Online || generation != m_generation)
                {
                    if (std::exchange(m_projectedReloadPending, false))
                        reloadProjectedWindows();
                    return;
                }
                if (!m_refreshGeneration.install(ticket, m_cacheEpoch))
                {
                    m_projectedReloadPending = false;
                    reloadProjectedWindows();
                    return;
                }

                if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                {
                    if (m_state.loadMoreInFlight)
                    {
                        m_state.loadMoreInFlight = false;
                        m_pendingLoadMoreOffset.reset();
                        m_pendingLoadMoreAnchor.reset();
                        m_pendingLoadMoreCommitted = false;
                        m_pendingLoadMoreRequestCompleted = false;
                        m_state.loadMoreError = error->message;
                    }
                    else
                    {
                        m_state.refreshError = error->message;
                    }
                    if (m_refreshAwaitingCache)
                    {
                        m_refreshAwaitingCache = false;
                        m_state.refreshInFlight = false;
                    }
                    Q_EMIT stateChanged();
                    if (std::exchange(m_projectedReloadPending, false))
                        reloadProjectedWindows();
                    return;
                }

                auto windows = std::get<ProjectedSearchWindows>(std::move(result));
                if (windows.size() != m_windows.size())
                {
                    m_state.stale = true;
                    Q_EMIT stateChanged();
                    return;
                }

                const auto pendingIt =
                    m_pendingLoadMoreOffset.has_value()
                        ? std::ranges::find_if(
                              m_windows, [this](const MessageListWindow& window)
                              { return window.requestedOffset == *m_pendingLoadMoreOffset; })
                        : m_windows.end();
                const auto pendingIndex =
                    pendingIt == m_windows.end()
                        ? windows.size()
                        : static_cast<std::size_t>(std::distance(m_windows.begin(), pendingIt));

                bool pendingCompatible = true;
                if (pendingIndex < windows.size() && !m_pendingLoadMoreCommitted &&
                    windows[pendingIndex].has_value() && pendingIndex > 0)
                {
                    pendingCompatible =
                        windows[pendingIndex]->queryState == m_windows[pendingIndex - 1].queryState;
                }

                bool allCurrent = pendingCompatible;
                for (std::size_t index = 0; index < windows.size(); ++index)
                {
                    if (!windows[index].has_value() || !displayCurrent(*windows[index]))
                    {
                        allCurrent = false;
                        m_windows[index].displayCurrent = false;
                    }
                }

                bool requestContinuation = false;
                bool materializeMissingInitial = false;
                std::size_t continuationOffset = 0;
                std::string continuationAnchor;
                if (allCurrent)
                {
                    std::vector<javelin::jmap::cache::SearchWindowPage> committed;
                    committed.reserve(windows.size());
                    for (auto& window : windows)
                        committed.push_back(std::move(*window));
                    rebuildFromProjectedWindows(std::move(committed));
                    const bool queryStateConsistent =
                        m_windows.empty() ||
                        std::ranges::all_of(m_windows, [queryState = m_windows.front().queryState](
                                                           const MessageListWindow& window)
                                            { return window.queryState == queryState; });
                    m_state.stale = !queryStateConsistent;
                    m_state.refreshError.clear();
                    m_state.loadMoreError.clear();
                    if (m_state.loadMoreInFlight && pendingIndex < windows.size())
                    {
                        m_state.loadMoreInFlight = false;
                        m_pendingLoadMoreOffset.reset();
                        m_pendingLoadMoreAnchor.reset();
                        m_pendingLoadMoreCommitted = false;
                        m_pendingLoadMoreRequestCompleted = false;
                    }
                }
                else if (pendingIndex < windows.size())
                {
                    const bool pendingCurrent = windows[pendingIndex].has_value() &&
                                                displayCurrent(*windows[pendingIndex]) &&
                                                pendingCompatible;
                    if (pendingCurrent)
                    {
                        auto page = std::move(*windows[pendingIndex]);
                        auto& metadata = m_windows[pendingIndex];
                        metadata.position = page.position;
                        metadata.returnedLimit = page.returnedLimit;
                        metadata.total = page.total;
                        metadata.queryState = page.queryState;
                        metadata.itemCount = page.items.size();
                        metadata.displayCurrent = true;

                        std::unordered_set<std::string> threadIds;
                        threadIds.reserve(m_state.items.size() + page.items.size());
                        for (const auto& item : m_state.items)
                            threadIds.insert(item.threadId);
                        for (auto& item : page.items)
                        {
                            if (threadIds.insert(item.threadId).second)
                                m_state.items.push_back(std::move(item));
                        }
                        m_state.itemsRevision = ++m_itemsRevision;
                        m_state.total = page.total;
                        m_endReached =
                            metadata.itemCount == 0 ||
                            (metadata.total.has_value() &&
                             metadata.position + metadata.itemCount >= *metadata.total) ||
                            (!metadata.total.has_value() && metadata.returnedLimit > 0 &&
                             metadata.itemCount < metadata.returnedLimit);
                        m_state.loadMoreInFlight = false;
                        m_state.loadMoreError.clear();
                        m_pendingLoadMoreOffset.reset();
                        m_pendingLoadMoreAnchor.reset();
                        m_pendingLoadMoreCommitted = false;
                        m_pendingLoadMoreRequestCompleted = false;
                        m_state.stale = true;
                    }
                    else if (!m_pendingLoadMoreCommitted && m_pendingLoadMoreAnchor.has_value())
                    {
                        continuationOffset = *m_pendingLoadMoreOffset;
                        continuationAnchor = *m_pendingLoadMoreAnchor;
                        m_windows.erase(m_windows.begin() +
                                        static_cast<std::ptrdiff_t>(pendingIndex));
                        requestContinuation = true;
                    }
                    else if (m_pendingLoadMoreRequestCompleted)
                    {
                        if (pendingIt != m_windows.end())
                            m_windows.erase(pendingIt);
                        m_state.loadMoreInFlight = false;
                        m_state.loadMoreError = i18n("Could not load more search results.");
                        m_pendingLoadMoreOffset.reset();
                        m_pendingLoadMoreAnchor.reset();
                        m_pendingLoadMoreCommitted = false;
                        m_pendingLoadMoreRequestCompleted = false;
                        m_state.stale = true;
                    }
                    else
                    {
                        m_state.stale = true;
                    }
                }
                else
                {
                    std::size_t currentPrefixLength = 0;
                    while (currentPrefixLength < windows.size() &&
                           windows[currentPrefixLength].has_value() &&
                           displayCurrent(*windows[currentPrefixLength]))
                    {
                        ++currentPrefixLength;
                    }

                    if (currentPrefixLength > 0)
                    {
                        std::vector<javelin::jmap::cache::SearchWindowPage> committed;
                        committed.reserve(currentPrefixLength);
                        for (std::size_t index = 0; index < currentPrefixLength; ++index)
                            committed.push_back(std::move(*windows[index]));
                        m_windows.resize(currentPrefixLength);
                        rebuildFromProjectedWindows(std::move(committed));
                        const bool queryStateConsistent = std::ranges::all_of(
                            m_windows, [queryState = m_windows.front().queryState](
                                           const MessageListWindow& window)
                            { return window.queryState == queryState; });
                        m_state.stale = !queryStateConsistent;
                        m_state.refreshError.clear();
                    }
                    else
                    {
                        m_state.stale = true;
                        if (m_state.items.empty())
                        {
                            m_state.cacheLoaded = false;
                            if (!m_refreshAwaitingCache && !m_state.refreshInFlight)
                                materializeMissingInitial = true;
                        }
                    }
                }

                bool refreshAgain = false;
                if (m_refreshAwaitingCache)
                {
                    m_refreshAwaitingCache = false;
                    m_state.refreshInFlight = false;
                    if (!m_state.cacheLoaded)
                        m_state.refreshError = i18n("Could not load the refreshed search results.");
                    refreshAgain = std::exchange(m_refreshAfterCurrent, false);
                }

                Q_EMIT stateChanged();
                if (requestContinuation)
                {
                    requestOnlineContinuation(continuationOffset, std::move(continuationAnchor));
                }
                else
                {
                    if (materializeMissingInitial)
                        requestOnlineInitial();
                    else if (allCurrent && !m_state.stale)
                        prefetchNextOnlineWindow();
                    if (refreshAgain)
                        requestOnlineInitial();
                }
                if (std::exchange(m_projectedReloadPending, false))
                    reloadProjectedWindows();
            });
        watcher->setFuture(QtConcurrent::run(loadProjectedSearchWindows, m_databasePath,
                                             m_accountId, onlineWindowKey(), std::move(requests)));
    }

    void SearchSession::rebuildFromProjectedWindows(
        std::vector<javelin::jmap::cache::SearchWindowPage> windows)
    {
        std::size_t itemCapacity = 0;
        for (const auto& window : windows)
            itemCapacity += window.items.size();

        std::vector<javelin::jmap::cache::MessageListItem> items;
        items.reserve(itemCapacity);
        std::unordered_set<std::string> threadIds;
        threadIds.reserve(itemCapacity);

        for (std::size_t index = 0; index < windows.size(); ++index)
        {
            auto& page = windows[index];
            auto& metadata = m_windows[index];
            metadata.requestedOffset = page.offset;
            metadata.requestedLimit = page.limit;
            metadata.position = page.position;
            metadata.returnedLimit = page.returnedLimit;
            metadata.total = page.total;
            metadata.queryState = page.queryState;
            metadata.itemCount = page.items.size();
            metadata.displayCurrent = true;
            for (auto& item : page.items)
            {
                if (threadIds.insert(item.threadId).second)
                    items.push_back(std::move(item));
            }
        }

        m_state.items = std::move(items);
        static_cast<void>(updateThreadMaterializationState());
        m_state.itemsRevision = ++m_itemsRevision;
        m_state.cacheLoaded = !m_windows.empty() && m_windows.front().displayCurrent;
        if (!m_windows.empty())
        {
            const auto& last = m_windows.back();
            m_state.total = last.total;
            m_endReached =
                last.itemCount == 0 ||
                (last.total.has_value() && last.position + last.itemCount >= *last.total) ||
                (!last.total.has_value() && last.returnedLimit > 0 &&
                 last.itemCount < last.returnedLimit);
        }
        else
        {
            m_state.total.reset();
            m_endReached = true;
        }
    }

    bool SearchSession::updateThreadMaterializationState()
    {
        const bool inFlight = std::ranges::any_of(
            m_state.items, [this](const javelin::jmap::cache::MessageListItem& item)
            { return m_materializingThreadIds.contains(item.threadId); });
        if (m_state.threadMaterializationInFlight == inFlight)
            return false;
        m_state.threadMaterializationInFlight = inFlight;
        return true;
    }

    void SearchSession::prefetchNextOnlineWindow()
    {
        if (m_closed || m_mode != SearchMode::Online || m_windows.empty() || m_endReached ||
            m_state.refreshInFlight)
        {
            return;
        }

        const auto& last = m_windows.back();
        if (!last.displayCurrent || last.itemCount == 0)
            return;
        const auto offset = nextOnlineOffset();
        if (last.total.has_value() && offset >= *last.total)
            return;
        if (m_prefetchOffsets.contains(offset) ||
            std::ranges::any_of(m_windows, [offset](const MessageListWindow& window)
                                { return window.requestedOffset == offset; }))
        {
            return;
        }

        m_prefetchOffsets.insert(offset);
        const auto generation = m_generation;
        const auto expectedQueryState = last.queryState;
        auto task = m_materializationPort.requestSearchWindow(SearchWindowIntent{
            .accountId = m_accountId,
            .criteria = m_criteria,
            .offset = offset,
            .limit = m_windowSize,
            .sort = m_sort,
            .anchor = std::nullopt,
            .windowKey = onlineWindowKey(),
        });
        QCoro::connect(
            std::move(task), this,
            [this, generation, offset, expectedQueryState](SearchWindowResult result)
            {
                m_prefetchOffsets.erase(offset);
                if (m_closed || m_mode != SearchMode::Online || generation != m_generation)
                    return;

                const auto* summary = std::get_if<SearchWindowSummary>(&result);
                const bool usable = summary != nullptr && summary->queryState == expectedQueryState;
                if (m_pendingLoadMoreOffset == std::optional<std::size_t>{offset})
                {
                    if (!usable)
                    {
                        if (m_pendingLoadMoreAnchor.has_value())
                            requestOnlineContinuation(offset, *m_pendingLoadMoreAnchor);
                        return;
                    }
                    if (std::ranges::none_of(m_windows, [offset](const MessageListWindow& window)
                                             { return window.requestedOffset == offset; }))
                    {
                        m_windows.push_back({
                            .requestedOffset = offset,
                            .requestedLimit = m_windowSize,
                            .position = 0,
                            .returnedLimit = 0,
                            .total = std::nullopt,
                            .queryState = {},
                            .itemCount = 0,
                            .displayCurrent = false,
                        });
                    }
                    reloadProjectedWindows();
                }
            });
    }

    void SearchSession::refreshAfterMutation()
    {
        if (m_mode != SearchMode::Online)
            return;
        if (m_state.refreshInFlight)
        {
            m_refreshAfterCurrent = true;
            return;
        }
        requestOnlineInitial();
    }

    void SearchSession::close()
    {
        m_closed = true;
        ++m_generation;
        ++m_refreshRequestId;
        m_refreshGeneration.close();
        if (m_mode == SearchMode::Online)
            m_materializationPort.retireSearchWindow(m_accountId, onlineWindowKey());
    }

    void SearchSession::markStale()
    {
        if (m_mode == SearchMode::Online)
            m_state.stale = true;
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
        m_refreshGeneration.replaceScope();
        m_state = MessageListState{};
        m_state.itemsRevision = ++m_itemsRevision;
        m_state.stale = true;
        m_localSnapshot.clear();
        m_localSnapshotLoaded = false;
        m_localVisibleCount = 0;
        m_prefetchOffsets.clear();
        m_pendingLoadMoreOffset.reset();
        m_pendingLoadMoreAnchor.reset();
        m_pendingLoadMoreCommitted = false;
        m_pendingLoadMoreRequestCompleted = false;
        m_refreshAwaitingCache = false;
        m_endReached = false;
        if (m_mode == SearchMode::Online)
            resetOnlineWindows();
    }

    bool SearchSession::canLoadMore() const
    {
        if (m_mode == SearchMode::Local)
            return m_localSnapshotLoaded && m_localVisibleCount < m_localSnapshot.size();
        if (m_mode != SearchMode::Online || m_state.refreshInFlight || m_state.loadMoreInFlight ||
            m_windows.empty() || m_state.items.empty() || m_endReached)
        {
            return false;
        }
        const auto& last = m_windows.back();
        return last.itemCount > 0 && (!last.total.has_value() || nextOnlineOffset() < *last.total);
    }

    bool SearchSession::loadMore()
    {
        if (!canLoadMore())
            return false;

        if (m_mode == SearchMode::Local)
        {
            m_localVisibleCount =
                std::min(m_localVisibleCount + m_windowSize, m_localSnapshot.size());
            applyLocalVisibleRange();
            Q_EMIT stateChanged();
            return true;
        }

        const auto offset = nextOnlineOffset();
        m_pendingLoadMoreOffset = offset;
        m_pendingLoadMoreAnchor = m_state.items.back().emailId;
        m_pendingLoadMoreCommitted = false;
        m_pendingLoadMoreRequestCompleted = false;
        m_state.loadMoreInFlight = true;
        m_state.loadMoreError.clear();
        Q_EMIT stateChanged();

        if (m_prefetchOffsets.contains(offset))
            return true;

        m_windows.push_back({
            .requestedOffset = offset,
            .requestedLimit = m_windowSize,
            .position = 0,
            .returnedLimit = 0,
            .total = std::nullopt,
            .queryState = {},
            .itemCount = 0,
            .displayCurrent = false,
        });
        reloadProjectedWindows();
        return true;
    }

    void SearchSession::ensureThreadMaterialized(std::string threadId)
    {
        m_materializationPort.ensureThread(ThreadMaterializationIntent{
            .accountId = m_accountId,
            .threadId = std::move(threadId),
        });
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
                    m_state.refreshError = error->message;
                    Q_EMIT stateChanged();
                    return;
                }

                m_localSnapshot =
                    std::get<std::vector<javelin::jmap::cache::MessageListItem>>(std::move(result));
                m_localSnapshotLoaded = true;
                m_localVisibleCount = std::min(m_windowSize, m_localSnapshot.size());
                applyLocalVisibleRange();
                Q_EMIT stateChanged();
            });
        watcher->setFuture(QtConcurrent::run(runLocalSearch, m_databasePath, m_accountId,
                                             *m_criteria.text, m_sort));
    }

    void SearchSession::applyLocalVisibleRange()
    {
        if (m_mode != SearchMode::Local || !m_localSnapshotLoaded)
            return;
        const auto end = std::min(m_localVisibleCount, m_localSnapshot.size());
        m_state.items.assign(m_localSnapshot.begin(),
                             m_localSnapshot.begin() + static_cast<std::ptrdiff_t>(end));
        m_state.itemsRevision = ++m_itemsRevision;
        m_state.total = m_localSnapshot.size();
        m_state.cacheLoaded = true;
        m_state.refreshInFlight = false;
        m_state.loadMoreInFlight = false;
        m_state.stale = false;
        m_state.refreshError.clear();
        m_state.loadMoreError.clear();
        m_endReached = end >= m_localSnapshot.size();
    }

    std::vector<MessageListWindowRequest> SearchSession::windowRequests() const
    {
        if (m_mode != SearchMode::Online)
            return {};

        std::vector<MessageListWindowRequest> requests;
        requests.reserve(m_windows.size());
        for (const auto& window : m_windows)
        {
            if (window.itemCount == 0)
                continue;
            requests.push_back({
                .offset = window.requestedOffset,
                .limit = window.requestedLimit,
            });
        }
        return requests;
    }

    void SearchSession::resetOnlineWindows()
    {
        m_windows.clear();
        m_windows.push_back({
            .requestedOffset = 0,
            .requestedLimit = m_windowSize,
            .position = 0,
            .returnedLimit = 0,
            .total = std::nullopt,
            .queryState = {},
            .itemCount = 0,
            .displayCurrent = false,
        });
        m_endReached = false;
    }

    std::size_t SearchSession::nextOnlineOffset() const
    {
        if (m_windows.empty())
            return 0;
        const auto& last = m_windows.back();
        return last.position + last.itemCount;
    }

    std::string SearchSession::onlineWindowKey() const
    {
        return javelin::jmap::search::cacheKey(m_criteria, m_sort) + "|session:" + m_sessionId;
    }
} // namespace javelin::app
