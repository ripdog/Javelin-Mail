#pragma once

#include "jmap/cache/Database.h"
#include "jmap/calendar/CalendarTypes.h"

#include <QDateTime>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{
    struct CalendarNotificationCandidate
    {
        std::string key;
        std::string ownerAccountId;
        std::string accountId;
        std::string eventId;
        std::string occurrenceId;
        std::string alertId;
        std::string title;
        QDateTime startsAt;
        calendar::Alert alert;
    };

    class CalendarNotificationRepository
    {
      public:
        explicit CalendarNotificationRepository(DatabaseConnection& connection);

        [[nodiscard]] std::variant<std::vector<CalendarNotificationCandidate>, DatabaseError>
        claimDue(const QDateTime& now);
        [[nodiscard]] std::optional<QDateTime> nextTrigger() const;
        [[nodiscard]] std::optional<DatabaseError> dismiss(std::string_view key);
        [[nodiscard]] std::optional<DatabaseError> snooze(std::string_view key,
                                                          const QDateTime& until);

      private:
        DatabaseConnection& m_connection;
        std::optional<QDateTime> m_nextTrigger;
    };
} // namespace javelin::jmap::cache
