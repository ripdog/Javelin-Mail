#pragma once

#include "app/MailTransferRepository.h"
#include "jmap/OperationError.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QCoroTask>

#include <QString>

#include <cstddef>
#include <optional>
#include <string>
#include <variant>

namespace javelin::app
{
    class AccountConnectionProvider;
}
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

namespace javelin::app
{
    struct MailTransferExecutionSummary
    {
        std::string operationId;
        MailTransferStatus status = MailTransferStatus::Preparing;
        std::size_t completeItemCount = 0;
        std::size_t destinationConfirmedItemCount = 0;
        std::size_t failedItemCount = 0;
        std::size_t partialItemCount = 0;
        std::size_t unknownItemCount = 0;
        std::optional<QString> historyEntryId;
    };

    using MailTransferExecutionResult =
        std::variant<MailTransferExecutionSummary, javelin::jmap::OperationError>;

    class MailTransferExecutor
    {
      public:
        MailTransferExecutor(
            javelin::jmap::cache::DatabaseConnection& databaseConnection,
            javelin::jmap::api::AbstractTransport& resourceTransport,
            javelin::jmap::api::JmapMethodTransport& methodTransport,
            javelin::jmap::MessageContentClient& messageContentClient,
            const AccountConnectionProvider& connectionProvider,
            javelin::app::undo::MailTransferHistoryCoordinator* historyCoordinator = nullptr);

        [[nodiscard]] QCoro::Task<MailTransferExecutionResult> advance(std::string operationId);

      private:
        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::api::AbstractTransport& m_resourceTransport;
        javelin::jmap::api::JmapMethodTransport& m_methodTransport;
        javelin::jmap::MessageContentClient& m_messageContentClient;
        const AccountConnectionProvider& m_connectionProvider;
        javelin::app::undo::MailTransferHistoryCoordinator* m_historyCoordinator = nullptr;
    };

} // namespace javelin::app
