#pragma once

#include "app/MailImportApplicationPorts.h"

#include <QCoroTask>

#include <QObject>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
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
    class MailImportService final : public QObject, public MailImportPort
    {
      public:
        MailImportService(
            javelin::jmap::cache::DatabaseConnection& databaseConnection,
            javelin::jmap::api::AbstractTransport& resourceTransport,
            javelin::jmap::api::JmapMethodTransport& methodTransport,
            const AccountConnectionProvider& connectionProvider, WorkScheduler& workScheduler,
            std::function<void(std::string_view, std::string_view)> requestMailboxResync = {},
            QObject* parent = nullptr);

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
        std::function<void(std::string_view, std::string_view)> m_requestMailboxResync;
        std::unordered_set<std::string> m_runningOperations;
        bool m_pumpScheduled = false;
        bool m_backgroundEnabled = false;
    };
} // namespace javelin::app
