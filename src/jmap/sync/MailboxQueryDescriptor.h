#pragma once

#include <cstddef>
#include <string>

namespace javelin::jmap::sync
{

    struct MailboxQueryDescriptor
    {
        std::string mailboxId;
        std::string sortProperty = "receivedAt";
        bool isAscending = false;
        bool collapseThreads = true;
    };

    [[nodiscard]] std::string mailboxQueryKey(const MailboxQueryDescriptor& descriptor);
    [[nodiscard]] std::size_t materializedMailboxWindowOffset(std::size_t requestedOffset,
                                                              bool anchoredRequest,
                                                              std::size_t returnedPosition);

} // namespace javelin::jmap::sync
