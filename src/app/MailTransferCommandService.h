#pragma once

#include "app/MailTransferExecutor.h"
#include "app/MailTransferPlanning.h"
#include "app/MessageSelection.h"

#include <QCoroTask>

#include <optional>
#include <string>

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
namespace javelin::app::undo
{
    class MailTransferHistoryCoordinator;
}

namespace javelin::app
{
    class AccountConnectionProvider;
    class MailTransferWorkService;

    struct CrossAccountMailTransferIntent
    {
        std::string sourceAccountId;
        std::optional<std::string> sourceMailboxId;
        std::string destinationAccountId;
        std::string destinationMailboxId;
        MailTransferOperation operation = MailTransferOperation::Move;
        MessageSelection selection;
    };

    class MailTransferCommandService
    {
      public:
        MailTransferCommandService(
            javelin::jmap::cache::DatabaseConnection& databaseConnection,
            javelin::jmap::api::AbstractTransport& resourceTransport,
            javelin::jmap::api::JmapMethodTransport& methodTransport,
            javelin::jmap::MessageContentClient& messageContentClient,
            const AccountConnectionProvider& connectionProvider,
            javelin::app::undo::MailTransferHistoryCoordinator& historyCoordinator);

        [[nodiscard]] QCoro::Task<MailTransferExecutionResult>
        transfer(CrossAccountMailTransferIntent intent);
        void setWorkService(MailTransferWorkService* workService);

      private:
        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::api::AbstractTransport& m_resourceTransport;
        javelin::jmap::api::JmapMethodTransport& m_methodTransport;
        javelin::jmap::MessageContentClient& m_messageContentClient;
        const AccountConnectionProvider& m_connectionProvider;
        javelin::app::undo::MailTransferHistoryCoordinator& m_historyCoordinator;
        MailTransferWorkService* m_workService = nullptr;
    };

} // namespace javelin::app
