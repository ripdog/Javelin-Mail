#pragma once

#include <optional>
#include <string>

namespace javelin::jmap::sync
{

    struct RefreshNotificationCandidate
    {
        std::string emailId;
        std::string threadId;
        std::optional<std::string> subject;
        std::string receivedAt;
    };

} // namespace javelin::jmap::sync
