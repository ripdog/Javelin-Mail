#pragma once

#include "app/MailImportApplicationPorts.h"

#include <QCoroTask>

#include <QObject>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace javelin::app
{
    class AccountConnectionProvider;
    class WorkScheduler;
    struct MailImportItemRecord;
    struct MailImportOperationRecord;
} // namespace javelin::app
namespace javelin::jmap::api
{
    class AbstractTransport;
    class JmapMethodTransport;
} // namespace javelin::jmap::api
namespace javelin::jmap::cache
{
    class DatabaseConnection;
}

namespace javelin::app
{
    struct MailImportScheduling
    {
        std::function<void(std::function<void()>)> defer;
        std::function<void(std::chrono::milliseconds, std::function<void()>)> retry;
    };

    class MailImportService final : public QObject, public MailImportPort
    {
      public:
        MailImportService(
            javelin::jmap::cache::DatabaseConnection& databaseConnection,
            javelin::jmap::api::AbstractTransport& resourceTransport,
            javelin::jmap::api::JmapMethodTransport& methodTransport,
            const AccountConnectionProvider& connectionProvider, WorkScheduler& workScheduler,
            std::function<void(std::string_view, std::string_view)> requestMailboxResync = {},
            MailImportScheduling scheduling = {}, QObject* parent = nullptr);

        [[nodiscard]] QCoro::Task<MailImportStartResult>
        startImport(MailImportIntent intent) override;
        void restoreRecoverable();
        void networkBecameReachable();
        void authenticationBecameAvailable();

      private:
        void schedulePump();
        void pump();
        [[nodiscard]] std::optional<javelin::jmap::OperationError>
        ensureTracked(std::string_view operationId);
        void requeueWaiting(bool authentication);
        void scheduleTransientRetry(std::string operationId,
                                    const javelin::jmap::OperationError& error,
                                    bool authentication = false);
        void resetTransientRetry(std::string_view operationId);
        void requeueWaitingOperation(std::string_view operationId, bool authentication = false);
        [[nodiscard]] QCoro::Task<void> runOne(std::string operationId, std::string jobId);
        [[nodiscard]] QCoro::Task<void> advanceOne(std::string operationId, std::string jobId);
        [[nodiscard]] QCoro::Task<std::optional<javelin::jmap::OperationError>>
        prepareScan(MailImportOperationRecord operation, std::string jobId);
        [[nodiscard]] QCoro::Task<std::optional<javelin::jmap::OperationError>>
        resolveNextMailbox(MailImportOperationRecord operation, std::string jobId);
        [[nodiscard]] QCoro::Task<std::optional<javelin::jmap::OperationError>>
        processNextItem(MailImportOperationRecord operation, std::string jobId);
        [[nodiscard]] QCoro::Task<std::optional<javelin::jmap::OperationError>>
        reconcileUnknownItem(MailImportOperationRecord operation, MailImportItemRecord item,
                             std::string jobId);
        [[nodiscard]] std::optional<javelin::jmap::OperationError>
        finalizeImport(const MailImportOperationRecord& operation, std::string_view jobId);
        void requestOperationSynchronization(const MailImportOperationRecord& operation);

        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::api::AbstractTransport& m_resourceTransport;
        javelin::jmap::api::JmapMethodTransport& m_methodTransport;
        const AccountConnectionProvider& m_connectionProvider;
        WorkScheduler& m_workScheduler;
        struct TransientRetryState
        {
            std::size_t attempts = 0;
            std::uint64_t generation = 0;
            bool scheduled = false;
            bool authentication = false;
        };

        std::function<void(std::string_view, std::string_view)> m_requestMailboxResync;
        std::function<void(std::function<void()>)> m_defer;
        std::function<void(std::chrono::milliseconds, std::function<void()>)> m_retry;
        std::unordered_set<std::string> m_runningOperations;
        std::unordered_map<std::string, TransientRetryState> m_transientRetries;
        bool m_pumpScheduled = false;
        bool m_backgroundEnabled = false;
    };
} // namespace javelin::app
