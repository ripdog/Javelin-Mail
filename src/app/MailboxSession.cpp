#include "app/MailboxSession.h"

#include "jmap/sync/MailboxQueryDescriptor.h"

#include <QCoroTask>

#include <QFutureWatcher>
#include <QtConcurrentRun>

#include <algorithm>
#include <utility>
#include <variant>

namespace javelin::app
{
    namespace
    {
        using ProjectedMailboxPageResult =
            std::variant<std::optional<javelin::jmap::cache::MailboxWindowPage>,
                         javelin::jmap::cache::DatabaseError>;

        [[nodiscard]] ProjectedMailboxPageResult
        loadProjectedMailboxPage(const QString& databasePath, const std::string& accountId,
                                 const std::string& queryKey, const std::size_t offset,
                                 const std::size_t limit,
                                 const javelin::jmap::query::EmailListSort sort)
        {
            javelin::jmap::cache::ThreadConnectionFactory factory{
                {.connectionNamePrefix = QStringLiteral("javelin-projected-mailbox"),
                 .databasePath = databasePath}};
            auto connectionResult = factory.openForCurrentThread("snapshot");
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&connectionResult))
                return *error;
            auto connection =
                std::get<javelin::jmap::cache::DatabaseConnection>(std::move(connectionResult));
            javelin::jmap::cache::QueryService queryService{connection};
            return queryService.loadMailboxWindow(accountId, queryKey, offset, limit, sort);
        }
    } // namespace

    MailboxSession::MailboxSession(std::string accountId, std::string mailboxId, QString title,
                                   std::optional<std::string> role,
                                   javelin::jmap::query::EmailListSort sort,
                                   javelin::jmap::cache::QueryService& queryService,
                                   MailApplicationService& mailService, const std::size_t pageSize,
                                   std::optional<RestoredMailboxState> restored, QObject* parent)
        : MessageListSession(parent), m_accountId(std::move(accountId)),
          m_mailboxId(std::move(mailboxId)), m_title(std::move(title)), m_role(std::move(role)),
          m_sort(sort), m_queryService(queryService), m_mailService(mailService),
          m_pageSize(pageSize),
          m_observation(m_mailService.observeMailbox(m_accountId, m_mailboxId))
    {
        if (restored.has_value())
            m_page = std::move(restored->page);
        else
            m_page.returnedLimit = m_pageSize;

        connect(&m_mailService, &MailApplicationService::cacheCommitted, this,
                [this](const MailCacheChange& change)
                {
                    if (change.accountId.toStdString() != m_accountId || m_page.refreshInFlight)
                        return;
                    for (const auto& window : change.queryWindows)
                    {
                        if (window.mailboxId.toStdString() == m_mailboxId &&
                            window.offset == m_page.offset)
                        {
                            reloadProjectedPage();
                            Q_EMIT pageChanged();
                            return;
                        }
                    }
                    for (const auto& changedMailboxId : change.mailboxIds)
                    {
                        if (changedMailboxId.toStdString() == m_mailboxId)
                        {
                            markStale();
                            reloadProjectedPage();
                            return;
                        }
                    }
                });
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

    const MessageListPage& MailboxSession::page() const
    {
        return m_page;
    }

    void MailboxSession::updateMetadata(QString title, std::optional<std::string> role)
    {
        m_title = std::move(title);
        m_role = std::move(role);
        Q_EMIT pageChanged();
    }

    void MailboxSession::loadCachedPage(const bool forceReload)
    {
        if (m_page.cacheLoaded && !forceReload)
            return;
        const auto result = m_queryService.loadMailboxWindow(m_accountId, queryKey(), m_page.offset,
                                                             m_pageSize, m_sort);
        const auto* page =
            std::get_if<std::optional<javelin::jmap::cache::MailboxWindowPage>>(&result);
        if (page == nullptr || !page->has_value())
        {
            m_page.cacheLoaded = false;
            m_page.stale = true;
            return;
        }
        applyCachedPage(**page);
    }

    void MailboxSession::reloadProjectedPage()
    {
        if (m_projectedReloadInFlight)
        {
            m_projectedReloadPending = true;
            return;
        }
        m_projectedReloadInFlight = true;
        const auto generation = m_generation;
        const auto offset = m_page.offset;
        auto* watcher = new QFutureWatcher<ProjectedMailboxPageResult>(this);
        connect(watcher, &QFutureWatcher<ProjectedMailboxPageResult>::finished, this,
                [this, watcher, generation, offset]
                {
                    auto result = watcher->result();
                    watcher->deleteLater();
                    m_projectedReloadInFlight = false;
                    if (generation == m_generation && offset == m_page.offset)
                    {
                        if (const auto* page =
                                std::get_if<std::optional<javelin::jmap::cache::MailboxWindowPage>>(
                                    &result);
                            page != nullptr && page->has_value())
                        {
                            applyCachedPage(**page);
                            Q_EMIT pageChanged();
                        }
                        else if (const auto* error =
                                     std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                        {
                            m_page.refreshError = error->message;
                            Q_EMIT pageChanged();
                        }
                    }
                    if (std::exchange(m_projectedReloadPending, false))
                        reloadProjectedPage();
                });
        watcher->setFuture(QtConcurrent::run(loadProjectedMailboxPage,
                                             m_queryService.databasePath(), m_accountId, queryKey(),
                                             m_page.offset, m_pageSize, m_sort));
    }

    void MailboxSession::applyCachedPage(javelin::jmap::cache::MailboxWindowPage page)
    {
        m_page.items = std::move(page.items);
        m_page.position = page.position;
        m_page.returnedLimit = page.returnedLimit;
        m_page.total = page.total;
        m_page.queryState = std::move(page.queryState);
        m_page.cacheLoaded =
            javelin::jmap::cache::isDisplayCurrent(page.coverage, page.materialization);
        m_page.stale = !javelin::jmap::cache::isDisplayCurrent(page.coverage, page.materialization);
    }

    void MailboxSession::refresh()
    {
        if (m_page.refreshInFlight)
            return;
        m_page.refreshInFlight = true;
        m_page.refreshError.clear();
        Q_EMIT pageChanged();
        const auto offset = m_page.offset;
        const auto generation = m_generation;
        auto task = m_mailService.requestMailboxWindow(MailboxWindowIntent{
            .accountId = m_accountId,
            .mailboxId = m_mailboxId,
            .offset = offset,
            .limit = m_pageSize,
            .sort = m_sort,
            .forceRefresh = m_page.stale,
            .anchor = m_page.anchor,
            .anchorOffset = m_anchorOffset,
        });
        QCoro::connect(
            std::move(task), this,
            [this, offset, generation](MailboxWindowResult result)
            {
                if (offset != m_page.offset || generation != m_generation)
                    return;
                m_page.refreshInFlight = false;
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    m_page.refreshError = error->message;
                    Q_EMIT pageChanged();
                    Q_EMIT refreshFailed(*error);
                    return;
                }

                const auto& summary = std::get<MailboxWindowSummary>(result);
                m_page.offset = summary.offset;
                m_page.total = summary.total;
                m_page.position = summary.position;
                m_page.returnedLimit = summary.returnedLimit;
                m_page.queryState = summary.queryState;
                m_page.anchor.reset();
                m_anchorOffset = 1;
                if (m_page.total.has_value() && m_page.offset > 0 &&
                    (*m_page.total == 0 || m_page.position >= *m_page.total))
                {
                    const auto step = m_page.returnedLimit == 0 ? m_pageSize : m_page.returnedLimit;
                    m_page.offset =
                        normalizedMessageListPageOffset(m_page.offset, *m_page.total, step);
                    resetForPageChange();
                    m_page.stale = true;
                    refresh();
                    return;
                }
                m_page.stale = false;
                m_page.refreshError.clear();
                loadCachedPage(true);
                Q_EMIT pageChanged();
            });
    }

    void MailboxSession::markStale()
    {
        m_page.stale = true;
    }

    void MailboxSession::setSort(javelin::jmap::query::EmailListSort sort)
    {
        if (m_sort.property == sort.property && m_sort.direction == sort.direction)
            return;
        m_sort = sort;
        m_page.offset = 0;
        m_page.total.reset();
        m_page.queryState.clear();
        m_page.stale = true;
        resetForPageChange();
    }

    void MailboxSession::reveal(std::string emailId)
    {
        if (m_page.refreshInFlight)
            return;
        m_page.anchor = std::move(emailId);
        m_anchorOffset = 0;
        m_page.stale = true;
        refresh();
    }

    bool MailboxSession::goToPage(const std::size_t pageIndex)
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

    bool MailboxSession::goToPreviousPage()
    {
        if (m_page.offset == 0)
            return false;
        const auto step = m_page.returnedLimit == 0 ? m_pageSize : m_page.returnedLimit;
        m_page.offset -= std::min(m_page.offset, step);
        m_page.anchor.reset();
        resetForPageChange();
        return true;
    }

    bool MailboxSession::goToNextPage()
    {
        if (m_page.total.has_value() && m_page.position + m_page.items.size() >= *m_page.total)
            return false;
        if (m_page.items.empty())
            return false;
        m_page.anchor = m_page.items.back().emailId;
        m_anchorOffset = 1;
        m_page.offset = m_page.position + m_page.items.size();
        resetForPageChange();
        return true;
    }

    void MailboxSession::resetForPageChange()
    {
        ++m_generation;
        m_page.position = m_page.offset;
        m_page.items.clear();
        m_page.cacheLoaded = false;
        m_page.refreshInFlight = false;
        m_page.refreshError.clear();
    }

    std::string MailboxSession::queryKey() const
    {
        return javelin::jmap::sync::mailboxQueryKey({
            .mailboxId = m_mailboxId,
            .sortProperty = javelin::jmap::query::propertyName(m_sort.property),
            .isAscending = javelin::jmap::query::isAscending(m_sort),
            .collapseThreads = true,
        });
    }
} // namespace javelin::app
