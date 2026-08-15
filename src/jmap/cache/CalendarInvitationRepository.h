#pragma once

#include "jmap/calendar/CalendarTypes.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QDateTime>

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{
    struct CalendarInvitationProjection
    {
        std::string eventId;
        std::optional<calendar::LocalDateTime> recurrenceId = std::nullopt;
        std::string selfParticipantId;
        std::optional<std::string> sourceNotificationId = std::nullopt;
        std::optional<calendar::LocalDateTime> displayRecurrenceId = std::nullopt;
        std::optional<calendar::LocalDateTime> displayStart = std::nullopt;
        std::optional<calendar::UtcInstant> displayUtcStart = std::nullopt;
        bool enqueueDesktopNotification = false;
    };

    struct CalendarInvitationReconciliation
    {
        std::string accountId;
        std::string notificationState;
        std::string eventState;
        bool replaceNotifications = false;
        std::vector<calendar::CalendarEventNotification> notifications;
        std::vector<std::string> deletedNotificationIds;
        std::vector<calendar::CalendarEvent> events;
        std::vector<calendar::Occurrence> nonRecurringOccurrences;
        std::vector<std::string> destroyedEventIds;
        std::vector<std::string> consideredEventIds;
        std::vector<CalendarInvitationProjection> pendingInvitations;
    };

    struct CalendarInvitationDispatchCandidate
    {
        std::string invitationKey;
        std::string ownerAccountId;
        std::string accountId;
        std::string eventId;
        std::string selfParticipantId;
        std::optional<std::string> sourceNotificationId;
        std::string title;
        std::string organizer;
        std::optional<std::string> location;
        calendar::LocalDateTime start;
        std::optional<calendar::UtcInstant> utcStart;
        std::optional<calendar::LocalDateTime> recurrenceId;
        std::optional<calendar::LocalDateTime> displayRecurrenceId;
        bool allDay = false;
        bool recurring = false;
    };

    class CalendarInvitationRepository
    {
      public:
        explicit CalendarInvitationRepository(DatabaseConnection& connection);

        [[nodiscard]] std::optional<DatabaseError>
        replaceParticipantIdentities(std::string_view accountId,
                                     const std::vector<calendar::ParticipantIdentity>& identities);
        [[nodiscard]] std::optional<DatabaseError>
        reconcile(const CalendarInvitationReconciliation& reconciliation);
        [[nodiscard]] std::variant<std::vector<std::string>, DatabaseError>
        pendingEventIds(std::string_view accountId) const;
        [[nodiscard]] std::variant<std::vector<CalendarInvitationDispatchCandidate>, DatabaseError>
        claimPendingDispatches();
        [[nodiscard]] std::optional<DatabaseError> markDelivered(std::string_view invitationKey,
                                                                 const QDateTime& deliveredAt);
        [[nodiscard]] std::optional<DatabaseError> releaseDispatch(std::string_view invitationKey);
        [[nodiscard]] std::optional<DatabaseError> recoverDispatches();
        [[nodiscard]] std::optional<DatabaseError> resolveEvent(std::string_view accountId,
                                                                std::string_view eventId);

      private:
        DatabaseConnection& m_connection;
    };
} // namespace javelin::jmap::cache
