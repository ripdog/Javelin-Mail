#pragma once

#include "jmap/EmailMutation.h"
#include "jmap/OperationError.h"
#include "jmap/sync/EmailMutationEngine.h"

#include <QCoroTask>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace javelin::app::undo
{
    struct RecreatedMailTransferSource
    {
        std::string emailId;
        std::optional<std::string> blobId;
        std::optional<std::string> threadId;
        std::optional<std::uint64_t> size;
    };

    using RecreatedMailTransferSourceResult =
        std::variant<RecreatedMailTransferSource, javelin::jmap::OperationError>;

    class MailTransferHistoryPort
    {
      public:
        virtual ~MailTransferHistoryPort() = default;

        [[nodiscard]] virtual QCoro::Task<javelin::jmap::AuthoritativeEmailsResult>
        getAuthoritativeEmails(std::string accountId, std::vector<std::string> emailIds) = 0;

        [[nodiscard]] virtual QCoro::Task<javelin::jmap::SubmittedEmailMutationsResult>
        applyExactEmailMutation(std::string accountId,
                                javelin::jmap::EmailMailboxMutation mutation) = 0;

        [[nodiscard]] virtual QCoro::Task<RecreatedMailTransferSourceResult> recreateSourceFromHistory(
            QString historyEntryId, std::string accountId, std::string rawContentHash,
            std::vector<std::string> mailboxIds, std::vector<std::string> keywords,
            std::vector<std::string> messageIds, std::optional<std::string> receivedAt,
            std::uint64_t sourceSize) = 0;
    };

} // namespace javelin::app::undo
