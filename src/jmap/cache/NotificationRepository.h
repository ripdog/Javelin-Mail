#pragma once

#include "jmap/cache/Database.h"
#include "jmap/sync/RefreshNotificationTypes.h"

#include <optional>
#include <string>
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
        enqueueUnreadMailboxEmails(std::string_view accountId, std::string_view mailboxId);

        [[nodiscard]] std::optional<DatabaseError>
        markDelivered(std::string_view accountId, std::string_view mailboxId,
                      const std::vector<std::string>& emailIds);

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
