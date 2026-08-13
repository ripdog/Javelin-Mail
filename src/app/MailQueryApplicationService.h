#pragma once

#include "app/MailApplicationPorts.h"
#include "app/MessageListMaterializationPort.h"
#include "jmap/sync/MailboxInterestRegistry.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QObject>
#include <QPointer>

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>

namespace javelin::jmap
{
    class MailQueryMaterializer;
}

namespace javelin::jmap::cache
{
    class ContactReader;
    class MailTagReader;
    class MailboxFilterReader;
    class MailboxMessageReader;
    class MailboxStatisticsReader;
} // namespace javelin::jmap::cache

namespace javelin::app
{
    class AccountRuntimeManager;
    class ApplicationErrorCoordinator;
    class MailboxMaintenanceRegistry;
    class ThreadMaterializationCoordinator;
    class WorkScheduler;
    class MailQueryApplicationService;

    class MailboxObservation final
    {
      public:
        MailboxObservation() = default;
        ~MailboxObservation();

        MailboxObservation(const MailboxObservation&) = delete;
        MailboxObservation& operator=(const MailboxObservation&) = delete;
        MailboxObservation(MailboxObservation&& other) noexcept;
        MailboxObservation& operator=(MailboxObservation&& other) noexcept;

        void reset();
        [[nodiscard]] explicit operator bool() const;

      private:
        friend class MailQueryApplicationService;

        MailboxObservation(
            MailQueryApplicationService& service,
            javelin::jmap::sync::MailboxInterestRegistry::ObservationId observationId);

        QPointer<MailQueryApplicationService> m_service;
        javelin::jmap::sync::MailboxInterestRegistry::ObservationId m_observationId = 0;
    };

    class MailQueryApplicationService final : public QObject,
                                              public MessageListMaterializationPort,
                                              public MailCacheChangePublisher
    {
        Q_OBJECT

      public:
        MailQueryApplicationService(
            javelin::jmap::cache::DatabaseConnection& databaseConnection,
            javelin::jmap::MailQueryMaterializer& queryMaterializer,
            javelin::jmap::cache::ContactReader& contactReader,
            javelin::jmap::cache::MailTagReader& mailTagReader,
            javelin::jmap::cache::MailboxStatisticsReader& mailboxStatisticsReader,
            javelin::jmap::cache::MailboxMessageReader& mailboxMessageReader,
            javelin::jmap::cache::MailboxFilterReader& mailboxFilterReader,
            AccountRuntimeManager& accountRuntime, ApplicationErrorCoordinator& errorCoordinator,
            WorkScheduler& workScheduler, MailboxMaintenanceRegistry& mailboxMaintenanceRegistry,
            QObject* parent = nullptr);

        void setThreadMaterializationCoordinator(ThreadMaterializationCoordinator* coordinator);
        [[nodiscard]] MailboxObservation observeMailbox(std::string accountId,
                                                        std::string mailboxId);
        [[nodiscard]] MailboxObservationLease
        beginMailboxObservation(std::string accountId, std::string mailboxId) override;
        [[nodiscard]] QCoro::Task<MailboxWindowResult>
        requestMailboxWindow(MailboxWindowIntent intent) override;
        [[nodiscard]] QCoro::Task<SearchWindowResult>
        requestSearchWindow(SearchWindowIntent intent) override;
        void ensureThread(ThreadMaterializationIntent intent) override;
        void retireSearchWindow(std::string accountId, std::string windowKey) override;
        void publishCacheChange(MailCacheChange change) override;
        void publishMailboxWindowCommitted(QString accountId, QString mailboxId, std::size_t offset,
                                           std::size_t limit);
        void publishThreadMaterializationCommitted(QString accountId, const QStringList& threadIds);

      Q_SIGNALS:
        void cacheCommitted(javelin::app::MailCacheChange change);
        void threadMaterializationProgress(javelin::app::ThreadMaterializationProgress progress);

      private:
        friend class MailboxObservation;
        void releaseMailboxObservation(
            javelin::jmap::sync::MailboxInterestRegistry::ObservationId observationId);
        [[nodiscard]] bool beginSearchWindowRequest(const std::string& leaseKey);
        void finishSearchWindowRequest(const std::string& leaseKey);
        [[nodiscard]] bool searchWindowRetired(const std::string& leaseKey) const;
        void publishObservedMailboxIds(const std::string& accountId);

        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::MailQueryMaterializer& m_queryMaterializer;
        javelin::jmap::cache::ContactReader& m_contactReader;
        javelin::jmap::cache::MailTagReader& m_mailTagReader;
        javelin::jmap::cache::MailboxStatisticsReader& m_mailboxStatisticsReader;
        javelin::jmap::cache::MailboxMessageReader& m_mailboxMessageReader;
        javelin::jmap::cache::MailboxFilterReader& m_mailboxFilterReader;
        AccountRuntimeManager& m_accountRuntime;
        ApplicationErrorCoordinator& m_errorCoordinator;
        WorkScheduler& m_workScheduler;
        MailboxMaintenanceRegistry& m_mailboxMaintenanceRegistry;
        ThreadMaterializationCoordinator* m_threadMaterializationCoordinator = nullptr;
        struct SearchWindowRequestState
        {
            std::size_t activeRequests = 0;
            bool retired = false;
        };
        std::unordered_map<std::string, SearchWindowRequestState> m_searchWindowRequests;
        javelin::jmap::sync::MailboxInterestRegistry m_mailboxInterests;
    };

} // namespace javelin::app
