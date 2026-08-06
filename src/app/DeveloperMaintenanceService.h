#pragma once

#include "app/DeveloperMaintenance.h"
#include "app/MailApplicationPorts.h"

#include <QThreadPool>

#include <QString>

#include <functional>
#include <string_view>

namespace javelin::app
{
    class MailboxMaintenanceRegistry;

    class DeveloperMaintenanceService final : public DeveloperMaintenancePort
    {
      public:
        DeveloperMaintenanceService(
            QString databasePath, QString vaultPath, MailboxMaintenanceRegistry& registry,
            MailCacheChangePublisher& cacheChangePublisher,
            std::function<void(std::string_view, std::string_view)> requestOfflineResync = {});
        ~DeveloperMaintenanceService() override;

        [[nodiscard]] QCoro::Task<DeveloperMailboxClearResult>
        clearMailboxCache(DeveloperMailboxClearCommand command) override;

      private:
        QString m_databasePath;
        QString m_vaultPath;
        MailboxMaintenanceRegistry& m_registry;
        MailCacheChangePublisher& m_cacheChangePublisher;
        std::function<void(std::string_view, std::string_view)> m_requestOfflineResync;
        QThreadPool m_workerPool;
    };
} // namespace javelin::app
