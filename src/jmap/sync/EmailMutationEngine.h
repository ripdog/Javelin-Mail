#pragma once

#include "jmap/EmailMutation.h"
#include "jmap/OperationError.h"
#include "jmap/api/LiveConnectionSettings.h"
#include "jmap/domain/MailEntities.h"
#include "jmap/sync/MutationCommitReceipt.h"

#include <QCoroTask>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{
    class DatabaseConnection;
}

namespace javelin::jmap::api
{
    class JmapMethodTransport;
}

namespace javelin::jmap
{
    struct MailCapabilityContext;

    struct SubmittedEmailMutations
    {
        struct Item
        {
            std::string emailId;
            std::vector<std::string> mutationIds;
            bool accepted = false;
            std::optional<std::string> error;
        };

        std::string accountId;
        std::size_t attemptedEmailCount = 0;
        std::size_t updatedEmailCount = 0;
        std::size_t failedEmailCount = 0;
        bool statePreconditionUsed = false;
        std::vector<Item> items;
        javelin::jmap::sync::MutationCommitReceipt receipt;
    };

    using SubmittedEmailMutationsResult = std::variant<SubmittedEmailMutations, OperationError>;

    struct AuthoritativeEmails
    {
        std::string accountId;
        std::string state;
        std::vector<javelin::jmap::domain::Email> emails;
        std::vector<std::string> notFound;
    };

    using AuthoritativeEmailsResult = std::variant<AuthoritativeEmails, OperationError>;

    class EmailMutationEngine
    {
      public:
        EmailMutationEngine(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                            javelin::jmap::api::JmapMethodTransport& methodTransport);
        ~EmailMutationEngine();

        [[nodiscard]] QueuedEmailMutationResult queue(std::string accountId,
                                                      EmailMailboxMutation mutation);
        [[nodiscard]] QueuedEmailMutationsResult
        queueBatch(std::string accountId, std::vector<EmailMailboxMutation> mutations);
        [[nodiscard]] QCoro::Task<SubmittedEmailMutationsResult>
        submitPending(LiveConnectionSettings settings, std::string accountId,
                      std::optional<std::string> operationGroupId = std::nullopt,
                      std::size_t limit = 25,
                      std::optional<std::string> ifInStateOverride = std::nullopt);
        [[nodiscard]] QCoro::Task<AuthoritativeEmailsResult>
        getAuthoritative(LiveConnectionSettings settings, std::string accountId,
                         std::vector<std::string> emailIds);

      private:
        std::unique_ptr<MailCapabilityContext> m_impl;
    };

} // namespace javelin::jmap
