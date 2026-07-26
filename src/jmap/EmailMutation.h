#pragma once

#include <optional>
#include <string>
#include <vector>

namespace javelin::jmap
{

    struct EmailMailboxMutation
    {
        std::string emailId;
        std::vector<std::string> addMailboxIds;
        std::vector<std::string> removeMailboxIds;
        std::vector<std::string> addKeywords;
        std::vector<std::string> removeKeywords;
        std::optional<std::string> operationGroupId;
        std::optional<std::string> ifInState;
        std::optional<std::vector<std::string>> authoritativeMailboxIds = std::nullopt;
        std::optional<std::vector<std::string>> authoritativeKeywords = std::nullopt;
    };

} // namespace javelin::jmap
