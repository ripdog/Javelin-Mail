#pragma once

#include "app/MailTransferExecutor.h"

#include <QCoroTask>

#include <QObject>

#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace javelin::app
{
    class AccountConnectionProvider;
    class WorkScheduler;
    enum class WorkStatus;
} // namespace javelin::app
namespace javelin::app::undo
{
    class MailTransferHistoryCoordinator;
}
namespace javelin::jmap
{
    class MessageContentClient;
}
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
    class MailTransferWorkService final : public QObject
    {
      public:
        using CompletionCallback = std::function<void(const MailTransferOperationRecord&)>;

        MailTransferWorkService(
            javelin::jmap::cache::DatabaseConnection& databaseConnection,
            javelin::jmap::api::AbstractTransport& resourceTransport,
            javelin::jmap::api::JmapMethodTransport& methodTransport,
            javelin::jmap::MessageContentClient& messageContentClient,
            const AccountConnectionProvider& connectionProvider,
            javelin::app::undo::MailTransferHistoryCoordinator& historyCoordinator,
            WorkScheduler& workScheduler, CompletionCallback completionCallback = {},
            QObject* parent = nullptr);

        [[nodiscard]] QCoro::Task<MailTransferExecutionResult>
        advanceForeground(std::string operationId);
        void networkBecameReachable();
        void authenticationBecameAvailable();
        void restoreRecoverable();

      private:
        void schedulePump();
        void pump();
        [[nodiscard]] QCoro::Task<void> runBackground(std::string operationId, std::string jobId);
        [[nodiscard]] QCoro::Task<MailTransferExecutionResult>
        executeAndTrack(std::string operationId, std::string jobId);
        void ensureTracked(std::string_view operationId);
        void repairCompletedHistory(const MailTransferOperationRecord& operation,
                                    std::string_view jobId);
        void requeueWaiting(WorkStatus status);

        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::api::AbstractTransport& m_resourceTransport;
        javelin::jmap::api::JmapMethodTransport& m_methodTransport;
        javelin::jmap::MessageContentClient& m_messageContentClient;
        const AccountConnectionProvider& m_connectionProvider;
        javelin::app::undo::MailTransferHistoryCoordinator& m_historyCoordinator;
        WorkScheduler& m_workScheduler;
        CompletionCallback m_completionCallback;
        std::unordered_set<std::string> m_runningOperations;
        bool m_pumpScheduled = false;
        bool m_backgroundEnabled = false;
    };

} // namespace javelin::app
