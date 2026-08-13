#include "app/ThreadMaterializationCoordinator.h"

#include <QCoroFuture>
#include <QCoroTask>

#include <QJsonArray>
#include <QJsonDocument>
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

        [[nodiscard]] QString jsonIds(const std::vector<std::string>& ids)
        {
            QJsonArray values;
            for (const auto& id : ids)
                values.push_back(QString::fromStdString(id));
            return QString::fromUtf8(QJsonDocument{values}.toJson(QJsonDocument::Compact));
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
        if (emailIds.empty())
            return std::nullopt;

        QSqlQuery query{m_databaseConnection.database()};
        query.prepare(QStringLiteral(
            "WITH requested AS MATERIALIZED (SELECT value AS email_id,CAST(key AS INTEGER) AS "
            "position FROM json_each(:email_ids_json)) SELECT e.thread_id FROM requested r INNER "
            "JOIN emails e ON e.account_id=:account_id AND e.email_id=r.email_id GROUP BY "
            "e.thread_id ORDER BY MIN(r.position)"));
        query.bindValue(QStringLiteral(":account_id"), QString::fromStdString(accountId));
        query.bindValue(QStringLiteral(":email_ids_json"), jsonIds(emailIds));
        if (!query.exec())
            return queryError(QStringLiteral("Resolve representative Threads"), query);

        std::vector<std::string> threadIds;
        threadIds.reserve(emailIds.size());
        while (query.next())
            threadIds.push_back(query.value(0).toString().toStdString());
        return ensureThreads(std::move(accountId), threadIds, priority);
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
    ThreadMaterializationCoordinator::enqueueRetainedMailbox(const std::string_view accountId,
                                                             const std::string_view mailboxId,
                                                             const WorkPriority priority)
    {
        QSqlQuery query{m_databaseConnection.database()};
        query.prepare(QStringLiteral(
            "SELECT i.email_id FROM mailbox_query_windows w INNER JOIN mailbox_query_window_items "
            "i "
            "ON i.account_id=w.account_id AND i.query_key=w.query_key AND "
            "i.requested_offset=w.requested_offset AND i.requested_limit=w.requested_limit WHERE "
            "w.account_id=:account_id AND w.mailbox_id=:mailbox_id AND w.coverage<>'stale' AND "
            "w.materialization='complete' ORDER BY w.requested_offset,i.position"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":mailbox_id"),
                        QString::fromStdString(std::string{mailboxId}));
        if (!query.exec())
            return queryError(QStringLiteral("Read retained mailbox materialization windows"),
                              query);
        std::vector<std::string> emailIds;
        while (query.next())
            emailIds.push_back(query.value(0).toString().toStdString());
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
    ThreadMaterializationCoordinator::restoreAccount(const std::string_view accountId,
                                                     const WorkPriority priority)
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
            "w.materialization='complete') represented ON represented.email_id=e.email_id WHERE "
            "e.account_id=:email_account"));
        const auto account = QString::fromStdString(std::string{accountId});
        query.bindValue(QStringLiteral(":mailbox_account"), account);
        query.bindValue(QStringLiteral(":search_account"), account);
        query.bindValue(QStringLiteral(":email_account"), account);
        if (!query.exec())
            return queryError(QStringLiteral("Restore Thread materialization targets"), query);
        std::vector<std::string> threadIds;
        while (query.next())
            threadIds.push_back(query.value(0).toString().toStdString());
        return ensureThreads(std::string{accountId}, threadIds, priority);
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    ThreadMaterializationCoordinator::ensureThreads(std::string accountId,
                                                    const std::vector<std::string>& threadIds,
                                                    const WorkPriority priority)
    {
        auto incompleteResult = incompleteThreadIds(accountId, threadIds);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&incompleteResult))
            return *error;
        return queueThreadIds(std::move(accountId),
                              std::get<std::vector<std::string>>(std::move(incompleteResult)),
                              priority);
    }

    std::variant<std::vector<std::string>, javelin::jmap::cache::DatabaseError>
    ThreadMaterializationCoordinator::incompleteThreadIds(
        const std::string_view accountId, const std::vector<std::string>& threadIds) const
    {
        if (threadIds.empty())
            return std::vector<std::string>{};

        QSqlQuery query{m_databaseConnection.database()};
        query.prepare(QStringLiteral(
            "WITH requested AS MATERIALIZED (SELECT value AS thread_id,CAST(key AS INTEGER) AS "
            "position FROM json_each(:thread_ids_json)), coverage AS (SELECT "
            "r.thread_id,r.position,"
            "t.membership_freshness,t.member_count,COUNT(tm.email_id) AS "
            "member_rows,COUNT(e.email_id) "
            "AS email_rows FROM requested r LEFT JOIN threads t ON t.account_id=:account_id AND "
            "t.thread_id=r.thread_id LEFT JOIN thread_email_members tm ON "
            "tm.account_id=t.account_id "
            "AND tm.thread_id=t.thread_id LEFT JOIN emails e ON e.account_id=tm.account_id AND "
            "e.email_id=tm.email_id AND e.thread_id=tm.thread_id AND NOT EXISTS(SELECT 1 FROM "
            "email_summary_refresh_requests refresh WHERE refresh.account_id=tm.account_id AND "
            "refresh.email_id=tm.email_id) GROUP BY r.thread_id,r.position,t.membership_freshness,"
            "t.member_count) SELECT thread_id FROM coverage WHERE membership_freshness IS NULL OR "
            "membership_freshness<>'current' OR member_count<>member_rows OR "
            "member_count<>email_rows "
            "ORDER BY position"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":thread_ids_json"), jsonIds(threadIds));
        if (!query.exec())
            return queryError(QStringLiteral("Inspect Thread materialization state"), query);

        std::vector<std::string> incomplete;
        incomplete.reserve(threadIds.size());
        while (query.next())
            incomplete.push_back(query.value(0).toString().toStdString());
        return incomplete;
    }

    QCoro::Task<ThreadMaterializationResult> ThreadMaterializationCoordinator::waitForThreads(
        std::string accountId, std::vector<std::string> threadIds, const WorkPriority priority)
    {
        std::erase_if(threadIds, [](const std::string& threadId) { return threadId.empty(); });
        std::ranges::sort(threadIds);
        threadIds.erase(std::unique(threadIds.begin(), threadIds.end()), threadIds.end());
        auto incompleteResult = incompleteThreadIds(accountId, threadIds);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&incompleteResult))
            co_return javelin::jmap::operationError(*error);
        auto incomplete = std::get<std::vector<std::string>>(std::move(incompleteResult));
        if (incomplete.empty())
        {
            const auto completedThreadCount = threadIds.size();
            co_return ThreadMaterializationSummary{
                .threadIds = std::move(threadIds),
                .missingEmailIds = {},
                .completedThreadCount = completedThreadCount,
                .completedEmailCount = 0,
            };
        }

        auto promise = std::make_shared<QPromise<ThreadMaterializationResult>>();
        promise->start();
        auto future = promise->future();
        const auto accountKey = accountId;
        m_waiters[accountKey].push_back(Waiter{
            .threadIds = std::move(threadIds),
            .promise = promise,
        });
        if (const auto error =
                queueThreadIds(std::move(accountId), std::move(incomplete), priority))
        {
            auto& waiters = m_waiters[accountKey];
            std::erase_if(waiters,
                          [&promise](const Waiter& waiter) { return waiter.promise == promise; });
            if (waiters.empty())
                m_waiters.erase(accountKey);
            promise->addResult(javelin::jmap::operationError(*error));
            promise->finish();
        }
        co_return co_await qCoro(future).takeResult();
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
        for (auto& threadId : threadIds)
        {
            if (threadId.empty())
                continue;

            if (const auto active = queue.active.find(threadId); active != queue.active.end())
            {
                if (static_cast<int>(priority) <= static_cast<int>(active->second))
                    continue;
            }

            const auto [pending, inserted] = queue.pending.try_emplace(
                threadId, PendingThread{.priority = priority, .sequence = m_nextPendingSequence++});
            if (!inserted &&
                static_cast<int>(priority) > static_cast<int>(pending->second.priority))
                pending->second.priority = priority;
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

            std::optional<WorkPriority> highestPriority;
            for (const auto& [threadId, pending] : queue.pending)
            {
                Q_UNUSED(threadId);
                if (!highestPriority.has_value() ||
                    static_cast<int>(pending.priority) > static_cast<int>(*highestPriority))
                    highestPriority = pending.priority;
            }
            if (!highestPriority.has_value())
                continue;
            const auto priority = *highestPriority;
            const auto admissionId = std::string{"thread-materialization:"} + accountId;
            auto admission = m_workScheduler.admitTransient(admissionId, accountId, priority);
            if (!admission.has_value())
                continue;

            std::vector<std::pair<std::uint64_t, std::string>> ordered;
            ordered.reserve(queue.pending.size());
            for (const auto& [threadId, pending] : queue.pending)
            {
                if (pending.priority == priority)
                    ordered.emplace_back(pending.sequence, threadId);
            }
            std::ranges::sort(ordered, {}, &std::pair<std::uint64_t, std::string>::first);
            std::vector<std::string> threadIds;
            threadIds.reserve(ordered.size());
            for (auto& [sequence, threadId] : ordered)
            {
                Q_UNUSED(sequence);
                threadIds.push_back(std::move(threadId));
            }
            for (const auto& threadId : threadIds)
            {
                queue.pending.erase(threadId);
                queue.active.emplace(threadId, priority);
            }
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

            if (!found->second.pending.empty())
            {
                std::vector<std::string> followUpIds;
                followUpIds.reserve(found->second.pending.size());
                for (const auto& [threadId, pending] : found->second.pending)
                {
                    Q_UNUSED(pending);
                    followUpIds.push_back(threadId);
                }
                const auto incomplete = incompleteThreadIds(accountId, followUpIds);
                if (const auto* remaining = std::get_if<std::vector<std::string>>(&incomplete))
                {
                    const std::unordered_set<std::string> incompleteIds(remaining->begin(),
                                                                        remaining->end());
                    std::erase_if(found->second.pending, [&incompleteIds](const auto& entry)
                                  { return !incompleteIds.contains(entry.first); });
                }
            }
            if (found->second.active.empty() && found->second.pending.empty())
                m_accounts.erase(found);
        }
        const auto* error = std::get_if<javelin::jmap::OperationError>(&result);
        Q_EMIT materializationFinished(QString::fromStdString(accountId),
                                       qStringIds(requestedThreadIds), error == nullptr,
                                       error == nullptr ? QString{} : error->message);
        completeWaiters(accountId, requestedThreadIds, error);
        schedulePump();
    }

    void ThreadMaterializationCoordinator::completeWaiters(
        const std::string_view accountId, const std::vector<std::string>& requestedThreadIds,
        const javelin::jmap::OperationError* error)
    {
        const auto found = m_waiters.find(std::string{accountId});
        if (found == m_waiters.end())
            return;
        auto& waiters = found->second;
        for (auto iterator = waiters.begin(); iterator != waiters.end();)
        {
            const bool affected = std::ranges::any_of(
                requestedThreadIds, [&waiter = *iterator](const std::string& threadId)
                { return std::ranges::contains(waiter.threadIds, threadId); });
            auto incompleteResult = incompleteThreadIds(accountId, iterator->threadIds);
            if (const auto* databaseError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&incompleteResult))
            {
                iterator->promise->addResult(javelin::jmap::operationError(*databaseError));
                iterator->promise->finish();
                iterator = waiters.erase(iterator);
                continue;
            }
            if (!std::get<std::vector<std::string>>(incompleteResult).empty())
            {
                if (error != nullptr && affected)
                {
                    iterator->promise->addResult(*error);
                    iterator->promise->finish();
                    iterator = waiters.erase(iterator);
                    continue;
                }
                ++iterator;
                continue;
            }
            iterator->promise->addResult(ThreadMaterializationSummary{
                .threadIds = iterator->threadIds,
                .missingEmailIds = {},
                .completedThreadCount = iterator->threadIds.size(),
                .completedEmailCount = 0,
            });
            iterator->promise->finish();
            iterator = waiters.erase(iterator);
        }
        if (waiters.empty())
            m_waiters.erase(found);
    }

} // namespace javelin::app
