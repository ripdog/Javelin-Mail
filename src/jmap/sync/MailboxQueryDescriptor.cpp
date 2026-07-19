#include "jmap/sync/MailboxQueryDescriptor.h"

namespace javelin::jmap::sync
{

    std::string mailboxQueryKey(const MailboxQueryDescriptor& descriptor)
    {
        return "mailbox:" + descriptor.mailboxId + "|sort:" + descriptor.sortProperty + ":" +
               (descriptor.isAscending ? "asc" : "desc") +
               "|collapseThreads:" + (descriptor.collapseThreads ? "true" : "false");
    }

    std::size_t materializedMailboxWindowOffset(const std::size_t requestedOffset,
                                                const bool anchoredRequest,
                                                const std::size_t returnedPosition)
    {
        return anchoredRequest ? returnedPosition : requestedOffset;
    }

} // namespace javelin::jmap::sync
