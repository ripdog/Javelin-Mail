#pragma once

#include "jmap/sync/RefreshNotificationTypes.h"
#include "storage/sqlite/DatabaseConnection.h"

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

    struct MailNotificationPendingEvent
    {
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

        [[nodiscard]] std::variant<bool, DatabaseError>
        createEventIfUnconsumed(DatabaseTransaction& transaction, std::string_view accountId,
                                const MailNotificationEventInput& event);
        [[nodiscard]] std::variant<std::vector<MailNotificationPendingEvent>, DatabaseError>
        listPendingEvents(std::string_view accountId) const;
        [[nodiscard]] std::optional<DatabaseError>
        synchronizeMailboxHorizons(std::string_view accountId,
                                   const std::vector<std::string>& enabledMailboxIds,
                                   std::optional<std::string_view> currentEmailState);
        [[nodiscard]] std::variant<std::vector<std::string>, DatabaseError>
        mailboxHorizonsAtState(std::string_view accountId, std::string_view emailState) const;
        [[nodiscard]] std::optional<DatabaseError>
        advanceMailboxHorizons(DatabaseTransaction& transaction, std::string_view accountId,
                               std::string_view expectedEmailState, std::string_view newEmailState);

        [[nodiscard]] std::variant<std::vector<javelin::jmap::sync::RefreshNotificationCandidate>,
                                   DatabaseError>
        enqueueUnreadMailboxEmails(std::string_view accountId, std::string_view mailboxId);

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
