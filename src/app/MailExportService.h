#pragma once

#include "app/MailExportApplicationPorts.h"

#include <QCoroTask>

#include <QObject>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace javelin::app
{
    class AccountConnectionProvider;
    class WorkScheduler;
    struct MailExportOperationRecord;
} // namespace javelin::app
namespace javelin::jmap
{
    class MailQueryClient;
    class MessageContentClient;
} // namespace javelin::jmap
namespace javelin::jmap::api
{
    class JmapMethodTransport;
}
namespace javelin::jmap::cache
{
    class DatabaseConnection;
}

namespace javelin::app
{
    class MailExportService final : public QObject, public MailExportPort
    {
      public:
        MailExportService(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                          javelin::jmap::api::JmapMethodTransport& methodTransport,
                          javelin::jmap::MailQueryClient& mailQueryClient,
                          javelin::jmap::MessageContentClient& messageContentClient,
                          const AccountConnectionProvider& connectionProvider,
                          WorkScheduler& workScheduler, QObject* parent = nullptr);

        [[nodiscard]] QCoro::Task<MailExportStartResult>
        startExport(MailExportIntent intent) override;
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
        prepareManifest(MailExportOperationRecord operation, std::string jobId);
        [[nodiscard]] QCoro::Task<std::optional<javelin::jmap::OperationError>>
        writeNextItem(MailExportOperationRecord operation, std::string jobId);
        [[nodiscard]] std::optional<javelin::jmap::OperationError>
        finalizeExport(const MailExportOperationRecord& operation);

        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::api::JmapMethodTransport& m_methodTransport;
        javelin::jmap::MailQueryClient& m_mailQueryClient;
        javelin::jmap::MessageContentClient& m_messageContentClient;
        const AccountConnectionProvider& m_connectionProvider;
        WorkScheduler& m_workScheduler;
        std::unordered_set<std::string> m_runningOperations;
        bool m_pumpScheduled = false;
        bool m_backgroundEnabled = false;
    };
} // namespace javelin::app
