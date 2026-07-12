#include "jmap/sync/MailboxWindowPolicy.h"

#include <algorithm>

namespace javelin::jmap::sync
{

    namespace
    {
        [[nodiscard]] bool
        usesCanonicalMailboxOrder(const javelin::jmap::query::EmailListSort& sort)
        {
            return sort.property == javelin::jmap::query::EmailListSortProperty::ReceivedAt &&
                   sort.direction == javelin::jmap::query::EmailListSortDirection::Descending;
        }
    } // namespace

    bool cacheSatisfiesMailboxWindow(const MailboxWindowAvailability& availability)
    {
        if (availability.forceRefresh)
        {
            return false;
        }

        if (availability.serverRepresentativeCount == 0)
        {
            return true;
        }

        if (availability.cachedRepresentativeCount >= availability.serverRepresentativeCount ||
            availability.offset >= availability.serverRepresentativeCount)
        {
            return true;
        }

        if (!usesCanonicalMailboxOrder(availability.sort))
        {
            return false;
        }

        const auto remainingServerRows =
            availability.serverRepresentativeCount - availability.offset;
        const auto requestedCount =
            availability.offset + std::min(availability.limit, remainingServerRows);
        return availability.cachedRepresentativeCount >= requestedCount;
    }

    std::size_t cachedMailboxWindowSize(const MailboxWindowAvailability& availability)
    {
        const auto availableRows = std::min(availability.cachedRepresentativeCount,
                                            availability.serverRepresentativeCount);
        if (availability.offset >= availableRows)
        {
            return 0;
        }

        return std::min(availability.limit, availableRows - availability.offset);
    }

} // namespace javelin::jmap::sync
