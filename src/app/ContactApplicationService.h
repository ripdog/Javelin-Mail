#pragma once

#include "app/ContactApplicationPorts.h"
#include "app/MailApplicationTypes.h"

#include <QObject>

#include <string>
#include <unordered_set>

namespace javelin::jmap::cache
{
    class ContactRepository;
}

namespace javelin::jmap::contacts
{
    class ContactSyncEngine;
}

namespace javelin::app
{
    class AccountRuntimeManager;
    class ApplicationErrorCoordinator;
    class WorkScheduler;
    namespace undo
    {
        class UndoManager;
    }

    class ContactApplicationService final : public QObject, public ContactRefreshPort
    {
        Q_OBJECT

      public:
        ContactApplicationService(javelin::jmap::cache::ContactRepository& contactRepository,
                                  javelin::jmap::contacts::ContactSyncEngine& syncEngine,
                                  AccountRuntimeManager& accountRuntime,
                                  ApplicationErrorCoordinator& errorCoordinator,
                                  WorkScheduler& workScheduler, undo::UndoManager& undoManager,
                                  QObject* parent = nullptr);

        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactRefreshResult>
        requestContacts(std::string ownerAccountId) override;
        void scheduleRefresh(std::string ownerAccountId);
        void restoreRefreshJobs();

      Q_SIGNALS:
        void cacheCommitted(javelin::app::MailCacheChange change);

      private:
        void scheduleRefreshPump();
        void pumpRefreshes();
        [[nodiscard]] QCoro::Task<void> runRefresh(std::string ownerAccountId, std::string jobId);

        javelin::jmap::contacts::ContactSyncEngine& m_contactSyncEngine;
        AccountRuntimeManager& m_accountRuntime;
        ApplicationErrorCoordinator& m_errorCoordinator;
        WorkScheduler& m_workScheduler;
        undo::UndoManager& m_undoManager;
        std::unordered_set<std::string> m_pendingContactRefreshes;
        std::unordered_set<std::string> m_runningContactRefreshes;
        bool m_contactRefreshPumpScheduled = false;
    };

} // namespace javelin::app
