#include "jmap/sync/MailboxQueryDescriptor.h"

namespace javelin::jmap::sync
{

    std::string mailboxQueryKey(const MailboxQueryDescriptor& descriptor)
    {
        return "mailbox:" + descriptor.mailboxId + "|sort:" + descriptor.sortProperty + ":" +
               (descriptor.isAscending ? "asc" : "desc") + "|collapseThreads:" +
               (descriptor.collapseThreads ? "true" : "false") + "|limit:" +
               std::to_string(descriptor.limit) + "|offset:" + std::to_string(descriptor.offset);
    }

} // namespace javelin::jmap::sync
