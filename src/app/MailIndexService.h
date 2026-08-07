#pragma once

#include <QCoroTask>
#include <QObject>
#include <QThreadPool>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace javelin::app
{
    class WorkScheduler;
}
namespace javelin::jmap::cache
{
    class DatabaseConnection;
}

namespace javelin::app
{
    class MailIndexService final : public QObject
    {
        Q_OBJECT

      public:
        MailIndexService(javelin::jmap::cache::DatabaseConnection& connection,
                         WorkScheduler& scheduler, QObject* parent = nullptr);
        ~MailIndexService() override;

        void applyAccounts(std::vector<std::string> accountIds);
        void requestIndex(std::string_view accountId);

      private:
        void schedulePump();
        void pump();
        [[nodiscard]] QCoro::Task<void> runAccount(std::string accountId, std::string jobId);

        javelin::jmap::cache::DatabaseConnection& m_connection;
        WorkScheduler& m_scheduler;
        std::unordered_map<std::string, std::string> m_jobs;
        std::unordered_set<std::string> m_runningAccounts;
        QThreadPool m_workerPool;
        bool m_pumpScheduled = false;
    };
} // namespace javelin::app
