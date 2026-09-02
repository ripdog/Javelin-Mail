#pragma once

#include "jmap/calendar/CalendarTypes.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QDateTime>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{
    struct CalendarPushedAlert
    {
        std::string key;
        std::string ownerAccountId;
        std::string accountId;
        std::string eventId;
        std::string uid;
        std::optional<std::string> recurrenceId;
        std::string alertId;
    };

    struct CalendarNotificationCandidate
    {
        std::string key;
        std::string identityKey;
        std::string ownerAccountId;
        std::string accountId;
        std::string eventId;
        std::string occurrenceId;
        std::optional<std::string> recurrenceId;
        std::string alertId;
        std::string title;
        QDateTime startsAt;
        calendar::Alert alert;
        std::optional<CalendarPushedAlert> pushedAlert;
    };

    [[nodiscard]] std::string
    calendarNotificationIdentityKey(std::string_view accountId, std::string_view eventId,
                                    const std::optional<std::string>& recurrenceId,
                                    std::string_view alertId, const QDateTime& trigger);

    struct CalendarPushedAlertClaim
    {
        bool claimed = false;
        bool completed = false;
        std::optional<QDateTime> retryAt;
    };

    enum class CalendarNotificationEligibility
    {
        Eligible,
        NotEligible,
        MetadataMissing,
    };

    class CalendarNotificationRepository
    {
      public:
        explicit CalendarNotificationRepository(DatabaseConnection& connection);

        [[nodiscard]] std::variant<std::vector<CalendarNotificationCandidate>, DatabaseError>
        claimDue(const QDateTime& now);
        [[nodiscard]] std::variant<bool, DatabaseError>
        enqueuePushed(const CalendarPushedAlert& alert);
        [[nodiscard]] std::variant<std::vector<CalendarPushedAlert>, DatabaseError>
        pendingPushed(std::optional<std::string_view> ownerAccountId = std::nullopt) const;
        [[nodiscard]] std::optional<DatabaseError> removePushed(std::string_view key);
        [[nodiscard]] std::variant<CalendarPushedAlertClaim, DatabaseError>
        claimPushed(std::string_view key, const QDateTime& now);
        [[nodiscard]] std::variant<CalendarNotificationEligibility, DatabaseError>
        notificationEligibility(std::string_view accountId,
                                const calendar::CalendarEvent& event) const;
        [[nodiscard]] std::variant<std::optional<calendar::Alert>, DatabaseError>
        effectiveAlert(std::string_view accountId, const calendar::CalendarEvent& event,
                       std::string_view alertId) const;
        [[nodiscard]] std::optional<DatabaseError>
        markDelivered(std::string_view key, std::string_view identityKey,
                      const QDateTime& deliveredAt,
                      std::optional<std::string_view> pushedAlertKey = std::nullopt);
        [[nodiscard]] std::optional<DatabaseError> releaseDispatch(std::string_view identityKey);
        [[nodiscard]] std::optional<DatabaseError> recoverDispatches();
        [[nodiscard]] std::optional<QDateTime> nextTrigger() const;
        [[nodiscard]] std::optional<DatabaseError> dismiss(std::string_view key);
        [[nodiscard]] std::optional<DatabaseError>
        snooze(std::string_view key, const QDateTime& until,
               const std::optional<CalendarPushedAlert>& pushedAlert = std::nullopt);

      private:
        DatabaseConnection& m_connection;
        std::optional<QDateTime> m_nextTrigger;
    };
} // namespace javelin::jmap::cache
