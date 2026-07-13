#pragma once

#include "jmap/cache/Database.h"
#include "jmap/sync/RefreshNotificationTypes.h"

#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    class NotificationRepository
    {
      public:
        explicit NotificationRepository(DatabaseConnection& connection);

        [[nodiscard]] std::variant<std::vector<javelin::jmap::sync::RefreshNotificationCandidate>,
                                   DatabaseError>
        claimUnreadMailboxEmails(std::string_view accountId, std::string_view mailboxId);

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
