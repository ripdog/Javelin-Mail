#pragma once

#include "jmap/EmailMutation.h"
#include "jmap/cache/Database.h"

#include <optional>
#include <string>
#include <vector>

namespace javelin::jmap::sync
{

    [[nodiscard]] QueuedEmailMutationsResult
    queueEmailMutations(cache::DatabaseConnection& connection, std::string accountId,
                        std::vector<EmailMailboxMutation> mutations);
    [[nodiscard]] QueuedEmailMutationResult
    queueEmailMutation(cache::DatabaseConnection& connection, std::string accountId,
                       EmailMailboxMutation mutation);
    [[nodiscard]] QueuedEmailMutationResult
    queueMailboxEmailMutation(cache::DatabaseConnection& connection, std::string accountId,
                              std::string emailId, std::string sourceMailboxId,
                              std::string destinationMailboxId, bool removeSourceMailbox);
    [[nodiscard]] QueuedEmailMutationResult
    queueDestroyEmailMutation(cache::DatabaseConnection& connection, std::string accountId,
                              std::string emailId,
                              std::optional<std::string> operationGroupId = std::nullopt);
    [[nodiscard]] QueuedEmailMutationResult
    queueEmailKeywordMutation(cache::DatabaseConnection& connection, std::string accountId,
                              std::string emailId, std::string keyword, bool enabled);

} // namespace javelin::jmap::sync
