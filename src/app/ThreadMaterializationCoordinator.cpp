#include "app/ThreadMaterializationCoordinator.h"

#include <QCoroTask>

#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>

#include <algorithm>
#include <ranges>
#include <utility>

namespace javelin::app
{
    namespace
    {
        [[nodiscard]] javelin::jmap::cache::DatabaseError queryError(const QString& operation,
                                                                     const QSqlQuery& query)
        {
            return {
                .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                .message = operation + QStringLiteral(": ") + query.lastError().text(),
            };
        }

        [[nodiscard]] QStringList qStringIds(const std::vector<std::string>& ids)
        {
            QStringList result;
            result.reserve(static_cast<qsizetype>(ids.size()));
            for (const auto& id : ids)
                result.push_back(QString::fromStdString(id));
            return result;
        }
    } // namespace

    ThreadMaterializationCoordinator::ThreadMaterializationCoordinator(
        javelin::jmap::cache::DatabaseConnection& databaseConnection, WorkScheduler& workScheduler,
        ThreadMaterializationWorker* worker, QObject* parent)
        : QObject(parent), m_databaseConnection(databaseConnection), m_workScheduler(workScheduler),
          m_worker(worker)
    {
        connect(&m_workScheduler, &WorkScheduler::foregroundAvailabilityChanged, this,
                &ThreadMaterializationCoordinator::schedulePump);
    }

    void ThreadMaterializationCoordinator::setWorker(ThreadMaterializationWorker* worker)
    {
        m_worker = worker;
        schedulePump();
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    ThreadMaterializationCoordinator::enqueueRepresentativeEmails(
        std::string accountId, const std::vector<std::string>& emailIds,
        const WorkPriority priority)
    {
        QSqlQuery query{m_databaseConnection.database()};
        query.prepare(QStringLiteral(
            "SELECT e.thread_id FROM emails e LEFT JOIN threads t ON t.account_id=e.account_id "
            "AND t.thread_id=e.thread_id WHERE e.account_id=:account_id AND e.email_id=:email_id "
            "AND (t.thread_id IS NULL OR t.membership_freshness<>'current' OR EXISTS(SELECT 1 FROM "
            "thread_email_members tm LEFT JOIN emails child ON child.account_id=tm.account_id AND "
            "child.email_id=tm.email_id AND child.thread_id=tm.thread_id WHERE "
            "tm.account_id=e.account_id AND "
            "tm.thread_id=e.thread_id AND child.email_id IS NULL))"));
        std::vector<std::string> threadIds;
        threadIds.reserve(emailIds.size());
        for (const auto& emailId : emailIds)
        {
            query.bindValue(QStringLiteral(":account_id"), QString::fromStdString(accountId));
            query.bindValue(QStringLiteral(":email_id"), QString::fromStdString(emailId));
            if (!query.exec())
                return queryError(QStringLiteral("Resolve representative Thread"), query);
            if (query.next())
                threadIds.push_back(query.value(0).toString().toStdString());
            query.finish();
        }
        return queueThreadIds(std::move(accountId), std::move(threadIds), priority);
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    ThreadMaterializationCoordinator::enqueueMailboxWindow(const std::string_view accountId,
                                                           const std::string_view queryKey,
                                                           const std::size_t offset,
                                                           const std::size_t limit,
                                                           const WorkPriority priority)
    {
        QSqlQuery query{m_databaseConnection.database()};
        query.prepare(
            QStringLiteral("SELECT i.email_id FROM mailbox_query_windows w INNER JOIN "
                           "mailbox_query_window_items i ON i.account_id=w.account_id AND "
                           "i.query_key=w.query_key AND i.requested_offset=w.requested_offset AND "
                           "i.requested_limit=w.requested_limit WHERE w.account_id=:account_id AND "
                           "w.query_key=:query_key AND w.requested_offset=:offset AND "
                           "w.requested_limit=:limit AND w.coverage<>'stale' AND "
                           "w.materialization='complete' ORDER BY i.position"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":query_key"),
                        QString::fromStdString(std::string{queryKey}));
        query.bindValue(QStringLiteral(":offset"), static_cast<qulonglong>(offset));
        query.bindValue(QStringLiteral(":limit"), static_cast<qulonglong>(limit));
        if (!query.exec())
            return queryError(QStringLiteral("Read mailbox materialization window"), query);
        std::vector<std::string> emailIds;
        while (query.next())
            emailIds.push_back(query.value(0).toString().toStdString());
        query.finish();
        return enqueueRepresentativeEmails(std::string{accountId}, emailIds, priority);
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    ThreadMaterializationCoordinator::enqueueSearchWindow(const std::string_view accountId,
                                                          const std::string_view queryKey,
                                                          const std::size_t offset,
                                                          const std::size_t limit,
                                                          const WorkPriority priority)
    {
        QSqlQuery query{m_databaseConnection.database()};
        query.prepare(QStringLiteral(
            "SELECT i.email_id FROM search_windows w INNER JOIN search_window_items i ON "
            "i.account_id=w.account_id AND i.query_key=w.query_key AND "
            "i.window_offset=w.window_offset AND i.window_limit=w.window_limit WHERE "
            "w.account_id=:account_id AND w.query_key=:query_key AND w.window_offset=:offset "
            "AND w.window_limit=:limit AND w.coverage<>'stale' AND "
            "w.materialization='complete' ORDER BY i.position"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":query_key"),
                        QString::fromStdString(std::string{queryKey}));
        query.bindValue(QStringLiteral(":offset"), static_cast<qulonglong>(offset));
        query.bindValue(QStringLiteral(":limit"), static_cast<qulonglong>(limit));
        if (!query.exec())
            return queryError(QStringLiteral("Read search materialization window"), query);
        std::vector<std::string> emailIds;
        while (query.next())
            emailIds.push_back(query.value(0).toString().toStdString());
        query.finish();
        return enqueueRepresentativeEmails(std::string{accountId}, emailIds, priority);
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    ThreadMaterializationCoordinator::restoreAccount(const std::string_view accountId)
    {
        QSqlQuery query{m_databaseConnection.database()};
        query.prepare(QStringLiteral(
            "SELECT DISTINCT e.thread_id FROM emails e INNER JOIN (SELECT i.email_id FROM "
            "mailbox_query_windows w INNER JOIN mailbox_query_window_items i ON "
            "i.account_id=w.account_id AND i.query_key=w.query_key AND "
            "i.requested_offset=w.requested_offset AND i.requested_limit=w.requested_limit "
            "WHERE w.account_id=:mailbox_account AND w.coverage<>'stale' AND "
            "w.materialization='complete' UNION SELECT i.email_id FROM search_windows w INNER "
            "JOIN search_window_items i ON i.account_id=w.account_id AND i.query_key=w.query_key "
            "AND i.window_offset=w.window_offset AND i.window_limit=w.window_limit WHERE "
            "w.account_id=:search_account AND w.coverage<>'stale' AND "
            "w.materialization='complete') represented ON represented.email_id=e.email_id LEFT "
            "JOIN threads t ON t.account_id=e.account_id AND t.thread_id=e.thread_id WHERE "
            "e.account_id=:email_account AND (t.thread_id IS NULL OR "
            "t.membership_freshness<>'current' OR EXISTS(SELECT 1 FROM thread_email_members tm "
            "LEFT JOIN emails child ON child.account_id=tm.account_id AND "
            "child.email_id=tm.email_id AND child.thread_id=tm.thread_id WHERE "
            "tm.account_id=e.account_id AND "
            "tm.thread_id=e.thread_id AND child.email_id IS NULL))"));
        const auto account = QString::fromStdString(std::string{accountId});
        query.bindValue(QStringLiteral(":mailbox_account"), account);
        query.bindValue(QStringLiteral(":search_account"), account);
        query.bindValue(QStringLiteral(":email_account"), account);
        if (!query.exec())
            return queryError(QStringLiteral("Restore Thread materialization targets"), query);
        std::vector<std::string> threadIds;
        while (query.next())
            threadIds.push_back(query.value(0).toString().toStdString());
        return queueThreadIds(std::string{accountId}, std::move(threadIds),
                              WorkPriority::Freshness);
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    ThreadMaterializationCoordinator::ensureThreads(std::string accountId,
                                                    const std::vector<std::string>& threadIds,
                                                    const WorkPriority priority)
    {
        QSqlQuery query{m_databaseConnection.database()};
        query.prepare(QStringLiteral(
            "SELECT membership_freshness,EXISTS(SELECT 1 FROM thread_email_members tm LEFT JOIN "
            "emails e ON e.account_id=tm.account_id AND e.email_id=tm.email_id AND "
            "e.thread_id=tm.thread_id WHERE "
            "tm.account_id=threads.account_id AND tm.thread_id=threads.thread_id AND "
            "e.email_id IS NULL) FROM threads WHERE account_id=:account_id AND "
            "thread_id=:thread_id"));
        std::vector<std::string> incompleteThreadIds;
        incompleteThreadIds.reserve(threadIds.size());
        for (const auto& threadId : threadIds)
        {
            query.bindValue(QStringLiteral(":account_id"), QString::fromStdString(accountId));
            query.bindValue(QStringLiteral(":thread_id"), QString::fromStdString(threadId));
            if (!query.exec())
                return queryError(QStringLiteral("Inspect Thread materialization state"), query);
            if (!query.next() || query.value(0).toString() != QStringLiteral("current") ||
                query.value(1).toBool())
                incompleteThreadIds.push_back(threadId);
            query.finish();
        }
        return queueThreadIds(std::move(accountId), std::move(incompleteThreadIds), priority);
    }

    std::size_t
    ThreadMaterializationCoordinator::pendingThreadCount(const std::string_view accountId) const
    {
        const auto found = m_accounts.find(std::string{accountId});
        return found == m_accounts.end() ? 0 : found->second.pending.size();
    }

    bool ThreadMaterializationCoordinator::isMaterializing(const std::string_view accountId,
                                                           const std::string_view threadId) const
    {
        const auto found = m_accounts.find(std::string{accountId});
        return found != m_accounts.end() &&
               (found->second.pending.contains(std::string{threadId}) ||
                found->second.active.contains(std::string{threadId}));
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    ThreadMaterializationCoordinator::queueThreadIds(std::string accountId,
                                                     std::vector<std::string> threadIds,
                                                     const WorkPriority priority)
    {
        auto& queue = m_accounts[accountId];
        queue.priority = static_cast<int>(priority) > static_cast<int>(queue.priority)
                             ? priority
                             : queue.priority;
        for (auto& threadId : threadIds)
        {
            if (!threadId.empty() && !queue.active.contains(threadId))
                queue.pending.insert(std::move(threadId));
        }
        if (queue.pending.empty() && queue.active.empty())
            m_accounts.erase(accountId);
        schedulePump();
        return std::nullopt;
    }

    void ThreadMaterializationCoordinator::schedulePump()
    {
        if (m_pumpScheduled)
            return;
        m_pumpScheduled = true;
        QTimer::singleShot(0, this,
                           [this]
                           {
                               m_pumpScheduled = false;
                               pump();
                           });
    }

    void ThreadMaterializationCoordinator::pump()
    {
        if (m_worker == nullptr)
            return;
        for (auto& [accountId, queue] : m_accounts)
        {
            if (queue.pending.empty() || !queue.active.empty())
                continue;
            const auto admissionId = std::string{"thread-materialization:"} + accountId;
            auto admission = m_workScheduler.admitTransient(admissionId, accountId, queue.priority);
            if (!admission.has_value())
                continue;

            std::vector<std::string> threadIds(queue.pending.begin(), queue.pending.end());
            std::ranges::sort(threadIds);
            queue.pending.clear();
            queue.active.insert(threadIds.begin(), threadIds.end());
            const auto priority = queue.priority;
            queue.priority = WorkPriority::Freshness;
            Q_EMIT materializationStarted(QString::fromStdString(accountId), qStringIds(threadIds));
            auto task = m_worker->materialize({
                .accountId = accountId,
                .threadIds = threadIds,
                .priority = priority,
            });
            QCoro::connect(
                std::move(task), this,
                [this, accountId, admissionId, threadIds](ThreadMaterializationResult result)
                { finish(accountId, admissionId, threadIds, std::move(result)); });
            return;
        }
    }

    void ThreadMaterializationCoordinator::finish(std::string accountId, std::string admissionId,
                                                  std::vector<std::string> requestedThreadIds,
                                                  ThreadMaterializationResult result)
    {
        m_workScheduler.release(admissionId);
        auto found = m_accounts.find(accountId);
        if (found != m_accounts.end())
        {
            for (const auto& threadId : requestedThreadIds)
                found->second.active.erase(threadId);
            if (found->second.active.empty() && found->second.pending.empty())
                m_accounts.erase(found);
        }
        const auto* error = std::get_if<javelin::jmap::OperationError>(&result);
        Q_EMIT materializationFinished(QString::fromStdString(accountId),
                                       qStringIds(requestedThreadIds), error == nullptr,
                                       error == nullptr ? QString{} : error->message);
        schedulePump();
    }

} // namespace javelin::app
