#pragma once

#include "app/MailApplicationTypes.h"
#include "app/MessageContentApplicationPorts.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QObject>

#include <string>

namespace javelin::jmap
{
    class MessageContentClient;
}

namespace javelin::app
{
    class AccountRuntimeManager;
    class ApplicationErrorCoordinator;
    class MailboxMaintenanceRegistry;
    class WorkScheduler;
    class ThreadMaterializationCoordinator;

    class MessageContentApplicationService final : public QObject
    {
        Q_OBJECT

      public:
        MessageContentApplicationService(
            javelin::jmap::cache::DatabaseConnection& databaseConnection,
            javelin::jmap::MessageContentClient& messageContentClient,
            AccountRuntimeManager& accountRuntime, ApplicationErrorCoordinator& errorCoordinator,
            WorkScheduler& workScheduler, MailboxMaintenanceRegistry& mailboxMaintenanceRegistry,
            QObject* parent = nullptr);

        [[nodiscard]] QCoro::Task<javelin::jmap::MessageContentRefreshResult>
        requestMessageContent(std::string accountId, std::string emailId);
        [[nodiscard]] QCoro::Task<javelin::jmap::AttachmentDownloadResult>
        requestAttachment(std::string accountId, std::string emailId, std::string partId);
        [[nodiscard]] QCoro::Task<javelin::jmap::MessageSourceDownloadResult>
        requestMessageSource(std::string accountId, std::string emailId);
        [[nodiscard]] QCoro::Task<SaveMessagesResult> saveMessages(SaveMessagesIntent intent);
        void setThreadMaterializationCoordinator(
            ThreadMaterializationCoordinator* threadMaterializationCoordinator);
        void publishMessageContentCommitted(QString accountId, QString emailId);

      Q_SIGNALS:
        void cacheCommitted(javelin::app::MailCacheChange change);

      private:
        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::MessageContentClient& m_messageContentClient;
        AccountRuntimeManager& m_accountRuntime;
        ApplicationErrorCoordinator& m_errorCoordinator;
        WorkScheduler& m_workScheduler;
        MailboxMaintenanceRegistry& m_mailboxMaintenanceRegistry;
        ThreadMaterializationCoordinator* m_threadMaterializationCoordinator = nullptr;
    };

} // namespace javelin::app
