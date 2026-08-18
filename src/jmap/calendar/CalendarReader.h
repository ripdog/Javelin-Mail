#pragma once

#include "jmap/OperationError.h"
#include "jmap/cache/CalendarRepository.h"
#include "jmap/calendar/CalendarTypes.h"

#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::calendar
{

    struct VisibleInterval
    {
        LocalDateTime start;
        LocalDateTime end;
    };

    using CalendarLoadResult = std::variant<std::optional<cache::CalendarWindow>, OperationError>;
    using CalendarAccountsResult =
        std::variant<std::vector<cache::CalendarAccount>, OperationError>;
    using CalendarListResult = std::variant<std::vector<Calendar>, OperationError>;
    using ParticipantIdentityListResult =
        std::variant<std::vector<ParticipantIdentity>, OperationError>;

    struct PendingCalendarInvitation
    {
        std::string ownerAccountId;
        std::string accountId;
        std::string eventId;
        std::string title;
        std::string organizer;
        LocalDateTime displayTime;
        std::optional<UtcInstant> displayUtc;
        std::optional<LocalDateTime> recurrenceId;
        std::optional<LocalDateTime> displayRecurrenceId;
        bool allDay = false;
        bool recurring = false;
        std::string selfParticipantId;
        std::string participationStatus;
        bool rsvpAllowed = false;
    };

    using PendingCalendarInvitationsResult =
        std::variant<std::vector<PendingCalendarInvitation>, OperationError>;
    using CalendarEventReadResult = std::variant<std::optional<CalendarEvent>, OperationError>;

    class CalendarReader
    {
      public:
        virtual ~CalendarReader() = default;

        [[nodiscard]] virtual CalendarLoadResult
        loadCached(std::string_view accountId, const VisibleInterval& interval,
                   const TimeZoneId& displayTimeZone) const = 0;
        [[nodiscard]] virtual CalendarLoadResult
        loadRangeSnapshot(std::string_view accountId, const VisibleInterval& interval,
                          const TimeZoneId& displayTimeZone) const = 0;
        [[nodiscard]] virtual CalendarAccountsResult accounts() const = 0;
        [[nodiscard]] virtual CalendarListResult calendars(std::string_view accountId) const = 0;
        [[nodiscard]] virtual ParticipantIdentityListResult
        participantIdentities(std::string_view accountId) const = 0;
        [[nodiscard]] virtual PendingCalendarInvitationsResult pendingInvitations() const = 0;
        [[nodiscard]] virtual CalendarEventReadResult event(std::string_view accountId,
                                                            std::string_view eventId) const = 0;
    };

} // namespace javelin::jmap::calendar
