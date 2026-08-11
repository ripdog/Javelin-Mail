#pragma once

#include "app/WorkScheduler.h"
#include "jmap/OperationError.h"
#include "jmap/cache/Database.h"

#include <QCoroTask>

#include <QObject>
#include <QStringList>

#include <cstddef>
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
        enqueueSearchWindow(std::string_view accountId, std::string_view queryKey,
                            std::size_t offset, std::size_t limit,
                            WorkPriority priority = WorkPriority::Freshness);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        restoreAccount(std::string_view accountId);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        ensureThreads(std::string accountId, const std::vector<std::string>& threadIds,
                      WorkPriority priority = WorkPriority::Interactive);
        [[nodiscard]] std::size_t pendingThreadCount(std::string_view accountId) const;
        [[nodiscard]] bool isMaterializing(std::string_view accountId,
                                           std::string_view threadId) const;

      Q_SIGNALS:
        void materializationStarted(QString accountId, QStringList threadIds);
        void materializationFinished(QString accountId, QStringList threadIds, bool successful,
                                     QString errorText);

      private:
        struct AccountQueue
        {
            std::unordered_set<std::string> pending;
            std::unordered_set<std::string> active;
            WorkPriority priority = WorkPriority::Freshness;
        };

        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        queueThreadIds(std::string accountId, std::vector<std::string> threadIds,
                       WorkPriority priority);
        void schedulePump();
        void pump();
        void finish(std::string accountId, std::string admissionId,
                    std::vector<std::string> requestedThreadIds,
                    ThreadMaterializationResult result);

        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        WorkScheduler& m_workScheduler;
        ThreadMaterializationWorker* m_worker = nullptr;
        std::unordered_map<std::string, AccountQueue> m_accounts;
        bool m_pumpScheduled = false;
    };

} // namespace javelin::app
