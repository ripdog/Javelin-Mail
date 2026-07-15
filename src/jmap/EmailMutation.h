#pragma once

#include <string>
#include <vector>

namespace javelin::jmap
{

    struct EmailMailboxMutation
    {
        std::string emailId;
        std::vector<std::string> addMailboxIds;
        std::vector<std::string> removeMailboxIds;
    };

} // namespace javelin::jmap
