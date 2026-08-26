#pragma once

#include "jmap/sync/RefreshNotificationTypes.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    struct MailNotificationEventInput
    {
        std::string mailboxId;
        std::string emailId;
        std::string threadId;
        std::optional<std::string> subject;
        std::string receivedAt;
    };

    struct MailNotificationEventRecord
    {
        std::int64_t eventId = 0;
        std::string accountId;
        std::string mailboxId;
        std::string emailId;
        std::string threadId;
        std::optional<std::string> subject;
        std::string receivedAt;
    };

    class NotificationRepository
    {
      public:
        explicit NotificationRepository(DatabaseConnection& connection);

        [[nodiscard]] std::variant<std::vector<javelin::jmap::sync::RefreshNotificationCandidate>,
                                   DatabaseError>
        enqueueUnreadMailboxEmails(std::string_view accountId, std::string_view mailboxId);

        [[nodiscard]] std::variant<std::int64_t, DatabaseError>
        enqueueEvent(DatabaseTransaction& transaction, std::string_view accountId,
                     const MailNotificationEventInput& event);
        [[nodiscard]] std::variant<std::vector<MailNotificationEventRecord>, DatabaseError>
        listEvents(std::string_view accountId) const;

        [[nodiscard]] std::optional<DatabaseError>
        markDelivered(std::string_view accountId, std::string_view mailboxId,
                      const std::vector<std::string>& emailIds);
        [[nodiscard]] std::optional<DatabaseError>
        releaseDispatches(std::string_view accountId, const std::vector<std::string>& emailIds);
        [[nodiscard]] std::optional<DatabaseError> recoverDispatches();

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
