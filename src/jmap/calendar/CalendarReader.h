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

    struct CalendarDayAccountSnapshot
    {
        cache::CalendarAccount account;
        std::vector<Calendar> calendars;
        std::vector<ParticipantIdentity> participantIdentities;
        cache::CalendarWindow window;
    };

    struct CalendarDaySnapshot
    {
        std::vector<CalendarDayAccountSnapshot> accounts;
        std::vector<PendingCalendarInvitation> pendingInvitations;
    };

    using CalendarDaySnapshotResult = std::variant<CalendarDaySnapshot, OperationError>;

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
        [[nodiscard]] virtual CalendarDaySnapshotResult
        daySnapshot(const VisibleInterval& interval, const TimeZoneId& displayTimeZone) const
        {
            auto accountResult = accounts();
            if (const auto* failure = std::get_if<OperationError>(&accountResult))
                return *failure;
            auto invitationResult = pendingInvitations();
            if (const auto* failure = std::get_if<OperationError>(&invitationResult))
                return *failure;

            CalendarDaySnapshot snapshot{
                .accounts = {},
                .pendingInvitations =
                    std::get<std::vector<PendingCalendarInvitation>>(std::move(invitationResult)),
            };
            auto accountValues =
                std::get<std::vector<cache::CalendarAccount>>(std::move(accountResult));
            snapshot.accounts.reserve(accountValues.size());
            for (auto& account : accountValues)
            {
                auto calendarResult = calendars(account.accountId);
                if (const auto* failure = std::get_if<OperationError>(&calendarResult))
                    return *failure;
                auto identityResult = participantIdentities(account.accountId);
                if (const auto* failure = std::get_if<OperationError>(&identityResult))
                    return *failure;
                auto rangeResult = loadRangeSnapshot(account.accountId, interval, displayTimeZone);
                if (const auto* failure = std::get_if<OperationError>(&rangeResult))
                    return *failure;
                auto window =
                    std::get<std::optional<cache::CalendarWindow>>(std::move(rangeResult));
                if (!window.has_value())
                    return OperationError{
                        .code = OperationErrorCode::LocalStorageFailure,
                        .message = QStringLiteral("The calendar day snapshot is unavailable.")};
                snapshot.accounts.push_back({
                    .account = std::move(account),
                    .calendars = std::get<std::vector<Calendar>>(std::move(calendarResult)),
                    .participantIdentities =
                        std::get<std::vector<ParticipantIdentity>>(std::move(identityResult)),
                    .window = std::move(*window),
                });
            }
            return snapshot;
        }
    };

} // namespace javelin::jmap::calendar
