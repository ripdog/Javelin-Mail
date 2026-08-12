#pragma once

#include "app/WorkScheduler.h"
#include "jmap/OperationError.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QCoroTask>

#include <QObject>
#include <QPromise>
#include <QStringList>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace javelin::app
{
    struct ThreadMaterializationTarget
    {
        std::string accountId;
        std::vector<std::string> threadIds;
        WorkPriority priority = WorkPriority::Freshness;
    };

    struct ThreadMaterializationSummary
    {
        std::vector<std::string> threadIds;
        std::vector<std::string> missingEmailIds;
        std::size_t completedThreadCount = 0;
        std::size_t completedEmailCount = 0;
    };

    using ThreadMaterializationResult =
        std::variant<ThreadMaterializationSummary, javelin::jmap::OperationError>;

    class ThreadMaterializationWorker
    {
      public:
        virtual ~ThreadMaterializationWorker() = default;
        [[nodiscard]] virtual QCoro::Task<ThreadMaterializationResult>
        materialize(ThreadMaterializationTarget target) = 0;
    };

    class ThreadMaterializationCoordinator final : public QObject
    {
        Q_OBJECT

      public:
        ThreadMaterializationCoordinator(
            javelin::jmap::cache::DatabaseConnection& databaseConnection,
            WorkScheduler& workScheduler, ThreadMaterializationWorker* worker = nullptr,
            QObject* parent = nullptr);

        void setWorker(ThreadMaterializationWorker* worker);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        enqueueRepresentativeEmails(std::string accountId, const std::vector<std::string>& emailIds,
                                    WorkPriority priority = WorkPriority::Freshness);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        enqueueMailboxWindow(std::string_view accountId, std::string_view queryKey,
                             std::size_t offset, std::size_t limit,
                             WorkPriority priority = WorkPriority::Freshness);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        enqueueRetainedMailbox(std::string_view accountId, std::string_view mailboxId,
                               WorkPriority priority = WorkPriority::Freshness);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        enqueueSearchWindow(std::string_view accountId, std::string_view queryKey,
                            std::size_t offset, std::size_t limit,
                            WorkPriority priority = WorkPriority::Freshness);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        restoreAccount(std::string_view accountId, WorkPriority priority = WorkPriority::Freshness);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        ensureThreads(std::string accountId, const std::vector<std::string>& threadIds,
                      WorkPriority priority = WorkPriority::Interactive);
        [[nodiscard]] QCoro::Task<ThreadMaterializationResult>
        waitForThreads(std::string accountId, std::vector<std::string> threadIds,
                       WorkPriority priority = WorkPriority::Interactive);
        [[nodiscard]] std::size_t pendingThreadCount(std::string_view accountId) const;
        [[nodiscard]] bool isMaterializing(std::string_view accountId,
                                           std::string_view threadId) const;

      Q_SIGNALS:
        void materializationStarted(QString accountId, QStringList threadIds);
        void materializationFinished(QString accountId, QStringList threadIds, bool successful,
                                     QString errorText);

      private:
        struct PendingThread
        {
            WorkPriority priority = WorkPriority::Freshness;
            std::uint64_t sequence = 0;
        };

        struct AccountQueue
        {
            std::unordered_map<std::string, PendingThread> pending;
            std::unordered_map<std::string, WorkPriority> active;
        };

        struct Waiter
        {
            std::vector<std::string> threadIds;
            std::shared_ptr<QPromise<ThreadMaterializationResult>> promise;
        };

        [[nodiscard]] std::variant<std::vector<std::string>, javelin::jmap::cache::DatabaseError>
        incompleteThreadIds(std::string_view accountId,
                            const std::vector<std::string>& threadIds) const;
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        queueThreadIds(std::string accountId, std::vector<std::string> threadIds,
                       WorkPriority priority);
        void schedulePump();
        void pump();
        void finish(std::string accountId, std::string admissionId,
                    std::vector<std::string> requestedThreadIds,
                    ThreadMaterializationResult result);
        void completeWaiters(std::string_view accountId,
                             const std::vector<std::string>& requestedThreadIds,
                             const javelin::jmap::OperationError* error);

        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        WorkScheduler& m_workScheduler;
        ThreadMaterializationWorker* m_worker = nullptr;
        std::unordered_map<std::string, AccountQueue> m_accounts;
        std::unordered_map<std::string, std::vector<Waiter>> m_waiters;
        std::uint64_t m_nextPendingSequence = 1;
        bool m_pumpScheduled = false;
    };

} // namespace javelin::app
