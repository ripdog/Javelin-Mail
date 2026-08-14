#pragma once

#include "storage/sqlite/DatabaseConnection.h"

#include <QCoroTask>
#include <QObject>

#include <cstdint>

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
    class LocalMaintenanceService final : public QObject
    {
        Q_OBJECT

      public:
        LocalMaintenanceService(javelin::jmap::cache::DatabaseConnection& connection,
                                WorkScheduler& scheduler, QObject* parent = nullptr);
        void requestReplay();

      private:
        [[nodiscard]] bool hasPendingMaintenance() const;
        void schedule();
        [[nodiscard]] QCoro::Task<void> run();

        javelin::jmap::cache::DatabaseConnection& m_connection;
        WorkScheduler& m_scheduler;
        bool m_scheduled = false;
        bool m_running = false;
        bool m_complete = false;
        bool m_replayRequested = true;
        std::uint64_t m_migrated = 0;
    };
} // namespace javelin::app
