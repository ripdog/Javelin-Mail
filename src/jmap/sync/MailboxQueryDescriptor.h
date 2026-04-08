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
        std::size_t limit = 100;
        std::size_t offset = 0;
    };

    [[nodiscard]] std::string mailboxQueryKey(const MailboxQueryDescriptor& descriptor);

} // namespace javelin::jmap::sync
