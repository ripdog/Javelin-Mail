#pragma once

#include "jmap/OperationError.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace javelin::jmap
{

    struct EmailMailboxMutation
    {
        std::string emailId;
        std::vector<std::string> addMailboxIds = {};
        std::vector<std::string> removeMailboxIds = {};
        std::vector<std::string> addKeywords = {};
        std::vector<std::string> removeKeywords = {};
        std::optional<std::string> operationGroupId = std::nullopt;
        std::optional<std::string> ifInState = std::nullopt;
        std::optional<std::vector<std::string>> authoritativeMailboxIds = std::nullopt;
        std::optional<std::vector<std::string>> authoritativeKeywords = std::nullopt;
        bool destroy = false;
    };

    struct QueuedEmailMutation
    {
        std::string mutationId;
        std::string accountId;
        std::string emailId;
        EmailMailboxMutation patch;
    };

    using QueuedEmailMutationResult = std::variant<QueuedEmailMutation, OperationError>;
    using QueuedEmailMutationsResult =
        std::variant<std::vector<QueuedEmailMutation>, OperationError>;

} // namespace javelin::jmap
