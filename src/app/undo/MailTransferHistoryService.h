#pragma once

#include "app/undo/MailTransferHistoryPort.h"
#include "storage/sqlite/DatabaseConnection.h"

namespace javelin::app
{
    class AccountConnectionProvider;
}
namespace javelin::jmap::api
{
    class AbstractTransport;
    class JmapMethodTransport;
} // namespace javelin::jmap::api

namespace javelin::app::undo
{
    class MailTransferHistoryService final : public MailTransferHistoryPort
    {
      public:
        MailTransferHistoryService(
            javelin::jmap::cache::DatabaseConnection& databaseConnection,
            javelin::jmap::api::AbstractTransport& resourceTransport,
            javelin::jmap::api::JmapMethodTransport& methodTransport,
            const javelin::app::AccountConnectionProvider& connectionProvider);

        [[nodiscard]] QCoro::Task<javelin::jmap::AuthoritativeEmailsResult>
        getAuthoritativeEmails(std::string accountId, std::vector<std::string> emailIds) override;

        [[nodiscard]] QCoro::Task<javelin::jmap::SubmittedEmailMutationsResult>
        applyExactEmailMutation(std::string accountId,
                                javelin::jmap::EmailMailboxMutation mutation) override;

        [[nodiscard]] QCoro::Task<RecreatedMailTransferSourceResult> recreateSourceFromHistory(
            QString historyEntryId, std::string accountId, std::string rawContentHash,
            std::vector<std::string> mailboxIds, std::vector<std::string> keywords,
            std::vector<std::string> messageIds, std::optional<std::string> receivedAt,
            std::uint64_t sourceSize) override;
        [[nodiscard]] QCoro::Task<RetainedMailTransferSourceResult>
        retainSourceForHistory(QString historyEntryId, std::string accountId,
                               std::string emailId) override;
        [[nodiscard]] QCoro::Task<RedoneMailTransferItemResult>
        redoMissingDestination(QString historyEntryId, MailTransferHistoryOperation operation,
                               std::string sourceAccountId, std::string destinationAccountId,
                               std::string destinationMailboxId,
                               MailTransferItemHistory item) override;

      private:
        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::api::AbstractTransport& m_resourceTransport;
        javelin::jmap::api::JmapMethodTransport& m_methodTransport;
        const javelin::app::AccountConnectionProvider& m_connectionProvider;
    };

} // namespace javelin::app::undo
