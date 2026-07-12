#pragma once

#include "jmap/query/EmailListSort.h"

#include <cstddef>

namespace javelin::jmap::sync
{

    struct MailboxWindowAvailability
    {
        std::size_t cachedRepresentativeCount = 0;
        std::size_t serverRepresentativeCount = 0;
        std::size_t offset = 0;
        std::size_t limit = 0;
        javelin::jmap::query::EmailListSort sort;
        bool forceRefresh = false;
    };

    [[nodiscard]] bool cacheSatisfiesMailboxWindow(const MailboxWindowAvailability& availability);
    [[nodiscard]] std::size_t
    cachedMailboxWindowSize(const MailboxWindowAvailability& availability);

} // namespace javelin::jmap::sync
