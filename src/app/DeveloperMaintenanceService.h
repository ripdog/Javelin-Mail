#pragma once

#include "app/DeveloperMaintenance.h"
#include "app/MailApplicationPorts.h"

#include <QObject>
#include <QThreadPool>

#include <QString>

#include <functional>
#include <string>
#include <string_view>

namespace javelin::app
{
    class MailboxMaintenanceRegistry;
    class WorkScheduler;

    class DeveloperMaintenanceService final : public QObject, public DeveloperMaintenancePort
    {
      public:
        DeveloperMaintenanceService(
            QString databasePath, QString vaultPath, MailboxMaintenanceRegistry& registry,
            MailCacheChangePublisher& cacheChangePublisher, WorkScheduler& workScheduler,
            std::function<void(std::string_view, std::string_view)> requestOfflineResync = {},
            QObject* parent = nullptr);
        ~DeveloperMaintenanceService() override;

        [[nodiscard]] QCoro::Task<DeveloperMailboxClearResult>
        clearMailboxCache(DeveloperMailboxClearCommand command) override;

      private:
        void schedulePump();
        void pump();
        [[nodiscard]] QCoro::Task<void> runJob(std::string jobId,
                                               DeveloperMailboxClearCommand command);

        QString m_databasePath;
        QString m_vaultPath;
        MailboxMaintenanceRegistry& m_registry;
        MailCacheChangePublisher& m_cacheChangePublisher;
        WorkScheduler& m_workScheduler;
        std::function<void(std::string_view, std::string_view)> m_requestOfflineResync;
        QThreadPool m_workerPool;
        bool m_pumpScheduled = false;
        bool m_running = false;
    };
} // namespace javelin::app
