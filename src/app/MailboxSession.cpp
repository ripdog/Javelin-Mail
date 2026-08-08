#include "app/MailboxSession.h"

#include "app/MailApplicationEventsPorts.h"
#include "jmap/cache/QueryService.h"
#include "jmap/sync/MailboxQueryDescriptor.h"

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
        struct WindowRequest
        {
            std::size_t offset = 0;
            std::size_t limit = 0;
        };

        struct ProjectedMailboxSnapshot
        {
            std::vector<std::optional<javelin::jmap::cache::MailboxWindowPage>> windows;
            std::optional<javelin::jmap::cache::MessageListItem> continuityItem;
        };

        using ProjectedMailboxSnapshotResult =
            std::variant<ProjectedMailboxSnapshot, javelin::jmap::cache::DatabaseError>;

        [[nodiscard]] ProjectedMailboxSnapshotResult loadProjectedMailboxWindows(
            const QString& databasePath, const std::string& accountId, const std::string& mailboxId,
            const std::string& queryKey, const std::vector<WindowRequest>& requests,
            const javelin::jmap::query::EmailListSort sort, const bool searchWindows,
            const std::optional<std::string>& continuityEmailId,
            const std::optional<std::string>& continuityThreadId)
        {
            javelin::jmap::cache::ReadOnlyThreadConnectionFactory factory{
                {.connectionNamePrefix = QStringLiteral("javelin-projected-mailbox"),
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
            ProjectedMailboxSnapshot snapshot;
            snapshot.windows.reserve(requests.size());
            for (const auto& request : requests)
            {
                if (searchWindows)
                {
                    auto result = queryService.loadSearchWindow(accountId, queryKey, request.offset,
                                                                request.limit);
                    if (const auto* error =
                            std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                    {
                        return *error;
                    }
                    auto page = std::get<std::optional<javelin::jmap::cache::SearchWindowPage>>(
                        std::move(result));
                    if (!page.has_value())
                    {
                        snapshot.windows.push_back(std::nullopt);
                        continue;
                    }
                    snapshot.windows.push_back(javelin::jmap::cache::MailboxWindowPage{
                        .requestedOffset = page->offset,
                        .requestedLimit = page->limit,
                        .position = page->position,
                        .returnedLimit = page->returnedLimit,
                        .total = page->total,
                        .queryState = std::move(page->queryState),
                        .coverage = page->coverage,
                        .materialization = page->materialization,
                        .items = std::move(page->items),
                    });
                    continue;
                }

                auto result = queryService.loadMailboxWindow(accountId, queryKey, request.offset,
                                                             request.limit, sort);
                if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                {
                    return *error;
                }
                snapshot.windows.push_back(
                    std::get<std::optional<javelin::jmap::cache::MailboxWindowPage>>(
                        std::move(result)));
            }

            if (searchWindows && continuityEmailId.has_value() && continuityThreadId.has_value())
            {
                const bool selectedThreadStillMatches = std::ranges::any_of(
                    snapshot.windows,
                    [&continuityThreadId](const auto& window)
                    {
                        return window.has_value() &&
                               std::ranges::any_of(
                                   window->items, [&continuityThreadId](const auto& item)
                                   { return item.threadId == *continuityThreadId; });
                    });
                if (!selectedThreadStillMatches)
                {
                    auto result = queryService.listMailboxThreadMessages(accountId, mailboxId,
                                                                         *continuityThreadId);
                    if (const auto* error =
                            std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                    {
                        return *error;
                    }
                    auto messages = std::get<std::vector<javelin::jmap::cache::MessageListItem>>(
                        std::move(result));
                    const auto selected =
                        std::ranges::find(messages, *continuityEmailId,
                                          &javelin::jmap::cache::MessageListItem::emailId);
                    if (selected != messages.end())
                    {
                        selected->threadMessageCount = messages.size();
                        snapshot.continuityItem = std::move(*selected);
                    }
                }
            }
            return snapshot;
        }

        [[nodiscard]] bool displayCurrent(const javelin::jmap::cache::MailboxWindowPage& page)
        {
            return javelin::jmap::cache::isDisplayCurrent(page.coverage, page.materialization);
        }
    } // namespace

    MailboxSession::MailboxSession(std::string accountId, std::string mailboxId, QString title,
                                   std::optional<std::string> role,
                                   javelin::jmap::query::EmailListSort sort,
                                   javelin::jmap::cache::QueryReader& queryReader,
                                   MessageListMaterializationPort& materializationPort,
                                   const std::size_t windowSize, MailApplicationEventsPort& events,
                                   std::optional<RestoredMailboxState> restored, QObject* parent)
        : MessageListSession(parent), m_accountId(std::move(accountId)),
          m_mailboxId(std::move(mailboxId)), m_title(std::move(title)), m_role(std::move(role)),
          m_sort(sort),
          m_quickFilterSessionId(QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString()),
          m_queryReader(queryReader), m_materializationPort(materializationPort), m_events(events),
          m_windowSize(windowSize),
          m_observation(m_materializationPort.beginMailboxObservation(m_accountId, m_mailboxId))
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
            resetToInitialWindow();

        connect(&m_events, &MailApplicationEventsPort::cacheInvalidated, this,
                [this](const MailCacheInvalidation& invalidation)
                {
                    const auto& change = invalidation.change;
                    if (change.accountId.toStdString() != m_accountId)
                        return;

                    m_cacheEpoch = std::max(m_cacheEpoch, invalidation.epoch);
                    static_cast<void>(m_refreshGeneration.begin(m_cacheEpoch));

                    if (quickFilterActive())
                    {
                        const auto key = quickFilterWindowKey();
                        const bool retainedFilteredWindowChanged = std::ranges::any_of(
                            change.searchWindows,
                            [this, &key](const SearchQueryWindowChange& changed)
                            {
                                return changed.queryKey.toStdString() == key &&
                                       std::ranges::any_of(
                                           m_windows,
                                           [&changed](const MessageListWindow& window)
                                           {
                                               return window.requestedOffset == changed.offset &&
                                                      window.requestedLimit == changed.limit;
                                           });
                            });
                        if (retainedFilteredWindowChanged)
                        {
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

                        if (change.mailboxIds.contains(QString::fromStdString(m_mailboxId)))
                        {
                            m_state.stale = true;
                            if (!m_state.refreshInFlight && !m_state.loadMoreInFlight)
                                requestInitialWindow(MessageListRefreshMode::Materialize,
                                                     std::nullopt, 1);
                        }
                        return;
                    }

                    bool retainedWindowChanged = false;
                    for (const auto& changed : change.queryWindows)
                    {
                        if (changed.mailboxId.toStdString() != m_mailboxId)
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

                    if (change.mailboxIds.contains(QString::fromStdString(m_mailboxId)))
                    {
                        m_state.stale = true;
                        reloadProjectedWindows();
                    }
                });
    }

    MailboxSession::~MailboxSession()
    {
        if (quickFilterActive())
            m_materializationPort.retireSearchWindow(m_accountId, quickFilterWindowKey());
    }

    const std::string& MailboxSession::accountId() const
    {
        return m_accountId;
    }

    const std::string& MailboxSession::mailboxId() const
    {
        return m_mailboxId;
    }

    QString MailboxSession::title() const
    {
        return m_title;
    }

    const std::optional<std::string>& MailboxSession::role() const
    {
        return m_role;
    }

    const MessageListState& MailboxSession::state() const
    {
        return m_state;
    }

    void MailboxSession::updateMetadata(QString title, std::optional<std::string> role)
    {
        m_title = std::move(title);
        m_role = std::move(role);
        Q_EMIT stateChanged();
    }

    void MailboxSession::loadCachedState(const bool forceReload)
    {
        if (m_state.cacheLoaded && !forceReload)
            return;
        reloadProjectedWindows();
    }

    void MailboxSession::reloadProjectedWindows()
    {
        if (m_projectedReloadInFlight)
        {
            m_projectedReloadPending = true;
            return;
        }
        if (m_windows.empty())
            resetToInitialWindow();

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
        const auto continuityEmailId = m_quickFilterContinuityEmailId;
        const auto continuityThreadId = m_quickFilterContinuityThreadId;
        auto* watcher = new QFutureWatcher<ProjectedMailboxSnapshotResult>(this);
        connect(
            watcher, &QFutureWatcher<ProjectedMailboxSnapshotResult>::finished, this,
            [this, watcher, generation, ticket, continuityEmailId, continuityThreadId]
            {
                auto result = watcher->result();
                watcher->deleteLater();
                m_projectedReloadInFlight = false;

                if (generation != m_generation ||
                    continuityEmailId != m_quickFilterContinuityEmailId ||
                    continuityThreadId != m_quickFilterContinuityThreadId ||
                    !m_refreshGeneration.install(ticket, m_cacheEpoch))
                {
                    if (std::exchange(m_projectedReloadPending, false))
                        reloadProjectedWindows();
                    return;
                }

                if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                {
                    if (m_state.loadMoreInFlight)
                    {
                        m_state.loadMoreInFlight = false;
                        m_pendingLoadMoreOffset.reset();
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

                auto snapshot = std::get<ProjectedMailboxSnapshot>(std::move(result));
                auto& windows = snapshot.windows;
                if (windows.size() != m_windows.size())
                {
                    m_state.stale = true;
                    Q_EMIT stateChanged();
                    return;
                }

                bool allCurrent = true;
                for (std::size_t index = 0; index < windows.size(); ++index)
                {
                    if (!windows[index].has_value() || !displayCurrent(*windows[index]))
                    {
                        allCurrent = false;
                        m_windows[index].displayCurrent = false;
                    }
                }

                bool materializeMissingInitial = false;
                if (allCurrent)
                {
                    std::vector<javelin::jmap::cache::MailboxWindowPage> committed;
                    committed.reserve(windows.size());
                    for (auto& window : windows)
                        committed.push_back(std::move(*window));
                    rebuildFromProjectedWindows(std::move(committed),
                                                std::move(snapshot.continuityItem));
                    const bool queryStateConsistent =
                        m_windows.empty() ||
                        std::ranges::all_of(m_windows, [queryState = m_windows.front().queryState](
                                                           const MessageListWindow& window)
                                            { return window.queryState == queryState; });
                    m_state.stale = !queryStateConsistent;
                    m_state.refreshError.clear();
                    m_state.loadMoreError.clear();
                    if (m_state.loadMoreInFlight)
                    {
                        m_state.loadMoreInFlight = false;
                        m_pendingLoadMoreOffset.reset();
                        m_pendingLoadMoreRequestCompleted = false;
                    }
                }
                else if (m_pendingLoadMoreOffset.has_value())
                {
                    const auto pending = std::ranges::find_if(
                        m_windows, [this](const MessageListWindow& window)
                        { return window.requestedOffset == *m_pendingLoadMoreOffset; });
                    const auto pendingIndex =
                        pending == m_windows.end()
                            ? windows.size()
                            : static_cast<std::size_t>(std::distance(m_windows.begin(), pending));
                    if (pendingIndex < windows.size() && windows[pendingIndex].has_value() &&
                        displayCurrent(*windows[pendingIndex]))
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
                        m_state.loadMoreError.clear();
                        m_pendingLoadMoreOffset.reset();
                        m_state.loadMoreInFlight = false;
                        m_pendingLoadMoreRequestCompleted = false;
                    }
                    else if (m_pendingLoadMoreRequestCompleted)
                    {
                        if (pending != m_windows.end())
                            m_windows.erase(pending);
                        m_state.loadMoreError =
                            i18n("Could not load more messages from the cache.");
                        m_pendingLoadMoreOffset.reset();
                        m_state.loadMoreInFlight = false;
                        m_pendingLoadMoreRequestCompleted = false;
                    }
                    m_state.stale = true;
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
                        std::vector<javelin::jmap::cache::MailboxWindowPage> committed;
                        committed.reserve(currentPrefixLength);
                        for (std::size_t index = 0; index < currentPrefixLength; ++index)
                            committed.push_back(std::move(*windows[index]));
                        m_windows.resize(currentPrefixLength);
                        rebuildFromProjectedWindows(std::move(committed),
                                                    std::move(snapshot.continuityItem));
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

                if (m_refreshAwaitingCache)
                {
                    m_refreshAwaitingCache = false;
                    m_state.refreshInFlight = false;
                    if (!m_state.cacheLoaded)
                        m_state.refreshError = i18n("Could not load the refreshed message list.");
                }

                Q_EMIT stateChanged();
                if (materializeMissingInitial)
                    refresh(MessageListRefreshMode::Materialize);
                if (std::exchange(m_projectedReloadPending, false))
                    reloadProjectedWindows();
            });
        watcher->setFuture(
            QtConcurrent::run(loadProjectedMailboxWindows, m_queryReader.databasePath(),
                              m_accountId, m_mailboxId, queryKey(), std::move(requests), m_sort,
                              quickFilterActive(), continuityEmailId, continuityThreadId));
    }

    void MailboxSession::rebuildFromProjectedWindows(
        std::vector<javelin::jmap::cache::MailboxWindowPage> windows,
        std::optional<javelin::jmap::cache::MessageListItem> continuityItem)
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
            metadata.requestedOffset = page.requestedOffset;
            metadata.requestedLimit = page.requestedLimit;
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

        m_quickFilterContinuityInjected = false;
        if (quickFilterActive() && continuityItem.has_value() &&
            m_quickFilterContinuityThreadId.has_value() &&
            threadIds.insert(*m_quickFilterContinuityThreadId).second)
        {
            const auto insertIndex = std::min(
                m_quickFilterContinuityPreferredIndex.value_or(items.size()), items.size());
            items.insert(items.begin() + static_cast<std::ptrdiff_t>(insertIndex),
                         std::move(*continuityItem));
            m_quickFilterContinuityPreferredIndex = insertIndex;
            m_quickFilterContinuityInjected = true;
        }
        else if (m_quickFilterContinuityThreadId.has_value())
        {
            const auto selected =
                std::ranges::find(items, *m_quickFilterContinuityThreadId,
                                  &javelin::jmap::cache::MessageListItem::threadId);
            if (selected != items.end())
            {
                m_quickFilterContinuityPreferredIndex =
                    static_cast<std::size_t>(std::distance(items.begin(), selected));
            }
        }

        m_state.items = std::move(items);
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

    void MailboxSession::refresh(const MessageListRefreshMode mode)
    {
        requestInitialWindow(mode, std::nullopt, 1);
    }

    void MailboxSession::requestInitialWindow(const MessageListRefreshMode mode,
                                              std::optional<std::string> anchor,
                                              const std::int64_t anchorOffset)
    {
        if (m_state.refreshInFlight)
            return;
        if (m_state.loadMoreInFlight)
        {
            ++m_refreshRequestId;
            if (m_pendingLoadMoreOffset.has_value())
            {
                const auto pending = std::ranges::find_if(
                    m_windows, [this](const MessageListWindow& window)
                    { return window.requestedOffset == *m_pendingLoadMoreOffset; });
                if (pending != m_windows.end())
                    m_windows.erase(pending);
            }
            m_pendingLoadMoreOffset.reset();
            m_state.loadMoreInFlight = false;
            m_pendingLoadMoreRequestCompleted = false;
        }

        m_state.refreshInFlight = true;
        m_state.refreshError.clear();
        m_state.loadMoreError.clear();
        m_state.stale = true;
        Q_EMIT stateChanged();

        const auto generation = m_generation;
        const auto requestId = ++m_refreshRequestId;
        const auto handleError =
            [this, generation, requestId](const javelin::jmap::OperationError& error)
        {
            if (generation != m_generation || requestId != m_refreshRequestId)
                return;
            m_state.refreshInFlight = false;
            m_state.refreshError = error.message;
            Q_EMIT stateChanged();
            Q_EMIT refreshFailed(error);
        };
        const auto handleSummary = [this, generation, requestId](const auto& summary)
        {
            if (generation != m_generation || requestId != m_refreshRequestId)
                return;
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
            m_state.loadMoreInFlight = false;
            m_pendingLoadMoreRequestCompleted = false;
            m_endReached = summary.representativeCount == 0 ||
                           (summary.total.has_value() &&
                            summary.position + summary.representativeCount >= *summary.total) ||
                           (!summary.total.has_value() && summary.returnedLimit > 0 &&
                            summary.representativeCount < summary.returnedLimit);
            m_refreshAwaitingCache = true;
            reloadProjectedWindows();
        };

        if (quickFilterActive())
        {
            auto task = m_materializationPort.requestSearchWindow(SearchWindowIntent{
                .accountId = m_accountId,
                .criteria = filteredCriteria(),
                .offset = 0,
                .limit = m_windowSize,
                .sort = m_sort,
                .anchor = std::move(anchor),
                .windowKey = quickFilterWindowKey(),
            });
            QCoro::connect(std::move(task), this,
                           [handleError, handleSummary](SearchWindowResult result)
                           {
                               if (const auto* error =
                                       std::get_if<javelin::jmap::OperationError>(&result))
                                   handleError(*error);
                               else
                                   handleSummary(std::get<SearchWindowSummary>(result));
                           });
            return;
        }

        auto task = m_materializationPort.requestMailboxWindow(MailboxWindowIntent{
            .accountId = m_accountId,
            .mailboxId = m_mailboxId,
            .offset = 0,
            .limit = m_windowSize,
            .sort = m_sort,
            .forceRefresh = mode == MessageListRefreshMode::RefreshFromServer,
            .anchor = std::move(anchor),
            .anchorOffset = anchorOffset,
        });
        QCoro::connect(std::move(task), this,
                       [handleError, handleSummary](MailboxWindowResult result)
                       {
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&result))
                               handleError(*error);
                           else
                               handleSummary(std::get<MailboxWindowSummary>(result));
                       });
    }

    void MailboxSession::markStale()
    {
        m_state.stale = true;
    }

    void MailboxSession::setSort(javelin::jmap::query::EmailListSort sort)
    {
        if (m_sort.property == sort.property && m_sort.direction == sort.direction)
            return;
        if (quickFilterActive())
            m_materializationPort.retireSearchWindow(m_accountId, quickFilterWindowKey());
        m_sort = sort;
        ++m_generation;
        ++m_refreshRequestId;
        m_refreshGeneration.replaceScope();
        m_refreshAwaitingCache = false;
        resetToInitialWindow();
        m_state.stale = true;
    }

    void MailboxSession::setQuickFilter(javelin::jmap::search::EmailSearchCriteria criteria)
    {
        criteria.inMailbox.reset();
        const auto oldActive = quickFilterActive();
        const auto oldKey = javelin::jmap::search::cacheKey(m_quickFilter, m_sort);
        const auto newKey = javelin::jmap::search::cacheKey(criteria, m_sort);
        if (oldKey == newKey)
            return;

        if (oldActive)
            m_materializationPort.retireSearchWindow(m_accountId, quickFilterWindowKey());
        m_quickFilter = std::move(criteria);
        m_quickFilterContinuityEmailId.reset();
        m_quickFilterContinuityThreadId.reset();
        m_quickFilterContinuityPreferredIndex.reset();
        m_quickFilterContinuityInjected = false;
        ++m_generation;
        ++m_refreshRequestId;
        m_refreshGeneration.replaceScope();
        m_refreshAwaitingCache = false;
        resetToInitialWindow();
        m_state.stale = true;
        Q_EMIT stateChanged();
        refresh(MessageListRefreshMode::Materialize);
    }

    void MailboxSession::setQuickFilterContinuitySelection(std::optional<std::string> emailId,
                                                           std::optional<std::string> threadId)
    {
        if (!quickFilterActive())
        {
            m_quickFilterContinuityEmailId.reset();
            m_quickFilterContinuityThreadId.reset();
            m_quickFilterContinuityPreferredIndex.reset();
            m_quickFilterContinuityInjected = false;
            return;
        }
        if (!emailId.has_value() || !threadId.has_value())
        {
            emailId.reset();
            threadId.reset();
        }
        if (m_quickFilterContinuityEmailId == emailId &&
            m_quickFilterContinuityThreadId == threadId)
        {
            return;
        }

        const bool hadInjectedContinuity = m_quickFilterContinuityInjected;
        m_quickFilterContinuityEmailId = std::move(emailId);
        m_quickFilterContinuityThreadId = std::move(threadId);
        m_quickFilterContinuityPreferredIndex.reset();
        if (m_quickFilterContinuityThreadId.has_value())
        {
            const auto selected =
                std::ranges::find(m_state.items, *m_quickFilterContinuityThreadId,
                                  &javelin::jmap::cache::MessageListItem::threadId);
            if (selected != m_state.items.end())
            {
                m_quickFilterContinuityPreferredIndex =
                    static_cast<std::size_t>(std::distance(m_state.items.begin(), selected));
            }
        }

        if (hadInjectedContinuity || m_projectedReloadInFlight)
            reloadProjectedWindows();
    }

    const javelin::jmap::search::EmailSearchCriteria& MailboxSession::quickFilter() const
    {
        return m_quickFilter;
    }

    bool MailboxSession::quickFilterActive() const
    {
        return !javelin::jmap::search::isEmpty(m_quickFilter);
    }

    void MailboxSession::reveal(std::string emailId)
    {
        requestInitialWindow(MessageListRefreshMode::Materialize, std::move(emailId), 0);
    }

    bool MailboxSession::canLoadMore() const
    {
        if (m_state.refreshInFlight || m_state.loadMoreInFlight || m_windows.empty() ||
            m_state.items.empty() || m_endReached)
        {
            return false;
        }
        const auto& last = m_windows.back();
        if (last.itemCount == 0)
            return false;
        return !last.total.has_value() || nextOffset() < *last.total;
    }

    bool MailboxSession::loadMore()
    {
        if (!canLoadMore())
            return false;

        const auto offset = nextOffset();
        auto anchorItem = m_state.items.crbegin();
        if (m_quickFilterContinuityInjected && m_quickFilterContinuityThreadId.has_value())
        {
            while (anchorItem != m_state.items.crend() &&
                   anchorItem->threadId == *m_quickFilterContinuityThreadId)
            {
                ++anchorItem;
            }
        }
        if (anchorItem == m_state.items.crend())
            return false;
        const auto anchor = anchorItem->emailId;
        const auto generation = m_generation;
        const auto requestId = ++m_refreshRequestId;
        m_pendingLoadMoreOffset = offset;
        m_pendingLoadMoreRequestCompleted = false;
        m_state.loadMoreInFlight = true;
        m_state.loadMoreError.clear();
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
        Q_EMIT stateChanged();

        const auto handleError =
            [this, generation, requestId, offset](const javelin::jmap::OperationError& error)
        {
            if (generation != m_generation || requestId != m_refreshRequestId ||
                !m_pendingLoadMoreOffset.has_value() || *m_pendingLoadMoreOffset != offset)
            {
                return;
            }
            const auto pending =
                std::ranges::find_if(m_windows, [offset](const MessageListWindow& window)
                                     { return window.requestedOffset == offset; });
            if (pending != m_windows.end())
                m_windows.erase(pending);
            m_pendingLoadMoreOffset.reset();
            m_pendingLoadMoreRequestCompleted = false;
            m_state.loadMoreInFlight = false;
            m_state.loadMoreError = error.message;
            Q_EMIT stateChanged();
        };
        const auto handleSummary = [this, generation, requestId, offset](const auto& summary)
        {
            if (generation != m_generation || requestId != m_refreshRequestId ||
                !m_pendingLoadMoreOffset.has_value() || *m_pendingLoadMoreOffset != offset)
            {
                return;
            }
            const auto pending =
                std::ranges::find_if(m_windows, [offset](const MessageListWindow& window)
                                     { return window.requestedOffset == offset; });
            if (pending == m_windows.end())
                return;
            pending->requestedOffset = summary.offset;
            pending->requestedLimit = summary.limit;
            pending->position = summary.position;
            pending->returnedLimit = summary.returnedLimit;
            pending->total = summary.total;
            pending->queryState = summary.queryState;
            pending->itemCount = summary.representativeCount;
            pending->displayCurrent = false;
            m_pendingLoadMoreRequestCompleted = true;
            m_endReached = summary.representativeCount == 0 ||
                           (summary.total.has_value() &&
                            summary.position + summary.representativeCount >= *summary.total) ||
                           (!summary.total.has_value() && summary.returnedLimit > 0 &&
                            summary.representativeCount < summary.returnedLimit);
            reloadProjectedWindows();
        };

        if (quickFilterActive())
        {
            auto task = m_materializationPort.requestSearchWindow(SearchWindowIntent{
                .accountId = m_accountId,
                .criteria = filteredCriteria(),
                .offset = offset,
                .limit = m_windowSize,
                .sort = m_sort,
                .anchor = anchor,
                .windowKey = quickFilterWindowKey(),
            });
            QCoro::connect(std::move(task), this,
                           [handleError, handleSummary](SearchWindowResult result)
                           {
                               if (const auto* error =
                                       std::get_if<javelin::jmap::OperationError>(&result))
                                   handleError(*error);
                               else
                                   handleSummary(std::get<SearchWindowSummary>(result));
                           });
            return true;
        }

        auto task = m_materializationPort.requestMailboxWindow(MailboxWindowIntent{
            .accountId = m_accountId,
            .mailboxId = m_mailboxId,
            .offset = offset,
            .limit = m_windowSize,
            .sort = m_sort,
            .forceRefresh = false,
            .anchor = anchor,
            .anchorOffset = 1,
        });
        QCoro::connect(std::move(task), this,
                       [handleError, handleSummary](MailboxWindowResult result)
                       {
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&result))
                               handleError(*error);
                           else
                               handleSummary(std::get<MailboxWindowSummary>(result));
                       });
        return true;
    }

    std::vector<MessageListWindowRequest> MailboxSession::windowRequests() const
    {
        if (quickFilterActive())
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

    void MailboxSession::resetToInitialWindow()
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
        m_pendingLoadMoreOffset.reset();
        m_pendingLoadMoreRequestCompleted = false;
        m_state = MessageListState{};
        m_state.itemsRevision = ++m_itemsRevision;
        m_endReached = false;
    }

    std::size_t MailboxSession::nextOffset() const
    {
        if (m_windows.empty())
            return 0;
        const auto& last = m_windows.back();
        return last.position + last.itemCount;
    }

    std::string MailboxSession::queryKey() const
    {
        if (quickFilterActive())
            return quickFilterWindowKey();
        return javelin::jmap::sync::mailboxQueryKey({
            .mailboxId = m_mailboxId,
            .sortProperty = javelin::jmap::query::propertyName(m_sort.property),
            .isAscending = javelin::jmap::query::isAscending(m_sort),
            .collapseThreads = true,
        });
    }

    std::string MailboxSession::quickFilterWindowKey() const
    {
        return "quick-filter:" + javelin::jmap::search::cacheKey(filteredCriteria(), m_sort) +
               "|session:" + m_quickFilterSessionId;
    }

    javelin::jmap::search::EmailSearchCriteria MailboxSession::filteredCriteria() const
    {
        auto criteria = m_quickFilter;
        criteria.inMailbox = m_mailboxId;
        return criteria;
    }
} // namespace javelin::app
