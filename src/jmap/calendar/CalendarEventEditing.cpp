#include "jmap/calendar/CalendarEventEditing.h"

#include <QUrl>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace
{
    bool startsWithMailto(const std::string_view value)
    {
        constexpr std::string_view prefix{"mailto:"};
        return value.size() >= prefix.size() &&
               std::equal(prefix.begin(), prefix.end(), value.begin(),
                          [](const char left, const char right)
                          {
                              return std::tolower(static_cast<unsigned char>(left)) ==
                                     std::tolower(static_cast<unsigned char>(right));
                          });
    }

    std::string trim(std::string value)
    {
        const auto whitespace = [](const unsigned char character)
        { return std::isspace(character); };
        const auto first = std::find_if_not(value.begin(), value.end(), whitespace);
        const auto last = std::find_if_not(value.rbegin(), value.rend(), whitespace).base();
        if (first >= last)
            return {};
        return {first, last};
    }

    std::string normalizedAddress(std::string address)
    {
        address = trim(std::move(address));
        if (startsWithMailto(address))
            address.erase(0, std::string_view{"mailto:"}.size());
        std::ranges::transform(address, address.begin(), [](const unsigned char character)
                               { return static_cast<char>(std::tolower(character)); });
        return address;
    }

    std::string normalizedCalendarAddress(const std::string_view address)
    {
        auto text =
            QString::fromUtf8(address.data(), static_cast<qsizetype>(address.size())).trimmed();
        QUrl url{text, QUrl::StrictMode};
        if (!url.isValid() || url.scheme().isEmpty())
            return normalizedAddress(text.toStdString());
        url.setScheme(url.scheme().toLower());
        if (!url.host().isEmpty())
            url.setHost(url.host().toLower());
        if (url.scheme() == QStringLiteral("mailto"))
            return normalizedAddress(url.path().toStdString());
        return url.toString(QUrl::FullyEncoded).toStdString();
    }

    struct ParsedAddress
    {
        std::string name;
        std::string address;
    };

    ParsedAddress parsedAddress(std::string value)
    {
        value = trim(std::move(value));
        const auto opening = value.rfind('<');
        if (opening != std::string::npos && value.ends_with('>') && opening + 1 < value.size() - 1)
        {
            auto name = trim(value.substr(0, opening));
            if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
                name = name.substr(1, name.size() - 2);
            return {.name = std::move(name),
                    .address = trim(value.substr(opening + 1, value.size() - opening - 2))};
        }
        if (startsWithMailto(value))
            value.erase(0, std::string_view{"mailto:"}.size());
        return {.name = {}, .address = std::move(value)};
    }

    std::optional<std::string> attendeeAddress(const javelin::jmap::calendar::Attendee& attendee)
    {
        auto address = attendee.email.value_or(attendee.calendarAddress);
        address = trim(std::move(address));
        if (address.empty())
            return std::nullopt;
        if (startsWithMailto(address))
            address.erase(0, std::string_view{"mailto:"}.size());
        return address;
    }
} // namespace

namespace javelin::jmap::calendar
{
    std::optional<std::size_t> participantIndexForAddress(const CalendarEvent& event,
                                                          const std::string_view calendarAddress)
    {
        const auto identity = normalizedCalendarAddress(calendarAddress);
        if (identity.empty())
            return std::nullopt;
        for (std::size_t index = 0; index < event.attendees.size(); ++index)
        {
            const auto& attendee = event.attendees[index];
            if (normalizedCalendarAddress(attendee.calendarAddress) == identity ||
                (attendee.email && normalizedAddress(*attendee.email) == identity))
                return index;
        }
        return std::nullopt;
    }

    bool eventOwnedByAddress(const CalendarEvent& event, const std::string_view calendarAddress)
    {
        const auto participant = participantIndexForAddress(event, calendarAddress);
        if (participant && event.attendees[*participant].isOwner)
            return true;
        return event.organizerCalendarAddress &&
               normalizedCalendarAddress(*event.organizerCalendarAddress) ==
                   normalizedCalendarAddress(calendarAddress);
    }

    bool eventHasOwner(const CalendarEvent& event)
    {
        return std::ranges::any_of(event.attendees,
                                   [](const Attendee& attendee) { return attendee.isOwner; });
    }

    bool eventInvitesAddress(const CalendarEvent& event, const std::string_view calendarAddress)
    {
        const auto participant = participantIndexForAddress(event, calendarAddress);
        return participant && !event.attendees[*participant].isOwner;
    }

    bool eventEditableWithRights(const CalendarEvent& event, const CalendarRights& rights,
                                 const std::string_view calendarAddress)
    {
        if (!event.isOrigin && eventInvitesAddress(event, calendarAddress))
            return false;
        if (rights.mayWriteAll)
            return true;
        return rights.mayWriteOwn &&
               (eventOwnedByAddress(event, calendarAddress) || !eventHasOwner(event));
    }

    std::vector<std::string> editableAttendeeAddresses(const std::vector<Attendee>& attendees)
    {
        std::vector<std::string> result;
        std::unordered_set<std::string> seen;
        for (const auto& attendee : attendees)
        {
            if (attendee.isOwner || !attendee.isAttendee)
                continue;
            const auto address = attendeeAddress(attendee);
            if (address && seen.emplace(normalizedAddress(*address)).second)
                result.push_back(*address);
        }
        return result;
    }

    std::vector<Attendee>
    reconcileEditableAttendees(const std::vector<Attendee>& existing,
                               const std::span<const std::string> requestedAddresses)
    {
        std::vector<Attendee> result;
        std::unordered_map<std::string, const Attendee*> byAddress;
        std::unordered_set<std::string> usedIds;
        for (const auto& attendee : existing)
        {
            usedIds.emplace(attendee.id);
            if (attendee.isOwner || !attendee.isAttendee)
            {
                result.push_back(attendee);
                continue;
            }
            if (const auto address = attendeeAddress(attendee))
                byAddress.try_emplace(normalizedAddress(*address), &attendee);
        }

        std::unordered_set<std::string> addedAddresses;
        std::size_t nextId = 1;
        for (const auto& requested : requestedAddresses)
        {
            const auto parsed = parsedAddress(requested);
            const auto normalized = normalizedAddress(parsed.address);
            if (normalized.empty() || !addedAddresses.emplace(normalized).second)
                continue;

            if (const auto found = byAddress.find(normalized); found != byAddress.end())
            {
                result.push_back(*found->second);
                continue;
            }

            std::string id;
            do
                id = "attendee-" + std::to_string(nextId++);
            while (usedIds.contains(id));
            usedIds.emplace(id);
            const auto name = parsed.name.empty() ? parsed.address : parsed.name;
            result.push_back({.id = std::move(id),
                              .name = name,
                              .email = parsed.address,
                              .calendarAddress = "mailto:" + parsed.address,
                              .participationStatus = "needs-action",
                              .isOwner = false,
                              .isAttendee = true,
                              .roles = {},
                              .expectReply = true,
                              .scheduleSequence = 0,
                              .scheduleUpdated = std::nullopt});
        }
        return result;
    }

    std::optional<CalendarEvent> effectiveOccurrenceEvent(const CalendarEvent& baseEvent,
                                                          const LocalDateTime& recurrenceId)
    {
        auto result = baseEvent;
        result.baseEventId = baseEvent.id;
        result.recurrenceId = recurrenceId;
        result.start = recurrenceId;
        result.utcStart.reset();
        result.utcEnd.reset();
        result.recurrenceRule.reset();
        result.recurrenceOverrides.clear();
        const auto found = baseEvent.recurrenceOverrides.find(recurrenceId.value);
        if (found == baseEvent.recurrenceOverrides.end())
            return result;
        const auto& occurrence = found->second;
        if (occurrence.excluded)
            return std::nullopt;
        if (occurrence.start)
            result.start = *occurrence.start;
        if (occurrence.duration)
            result.duration = *occurrence.duration;
        if (occurrence.title)
            result.title = *occurrence.title;
        for (const auto& [participantId, participant] : occurrence.participantOverrides)
        {
            const auto existing = std::ranges::find(result.attendees, participantId, &Attendee::id);
            if (existing == result.attendees.end())
                result.attendees.push_back(participant);
            else
                *existing = participant;
        }
        for (const auto& [participantId, status] : occurrence.participantParticipationStatus)
        {
            const auto participant =
                std::ranges::find(result.attendees, participantId, &Attendee::id);
            if (participant != result.attendees.end())
                participant->participationStatus = status;
        }
        return result;
    }

    CalendarEvent applyOccurrenceEdit(const CalendarEvent& baseEvent,
                                      const LocalDateTime& recurrenceId,
                                      const CalendarEvent& editedOccurrence)
    {
        auto result = baseEvent;
        auto& occurrence = result.recurrenceOverrides[recurrenceId.value];
        occurrence.excluded = false;
        occurrence.start = editedOccurrence.start.value == recurrenceId.value
                               ? std::nullopt
                               : std::optional{editedOccurrence.start};
        occurrence.duration = editedOccurrence.duration == baseEvent.duration
                                  ? std::nullopt
                                  : std::optional{editedOccurrence.duration};
        occurrence.title = editedOccurrence.title == baseEvent.title
                               ? std::nullopt
                               : std::optional{editedOccurrence.title};
        if (!occurrence.start && !occurrence.duration && !occurrence.title &&
            occurrence.participantOverrides.empty() &&
            occurrence.participantParticipationStatus.empty())
            result.recurrenceOverrides.erase(recurrenceId.value);
        return result;
    }

    CalendarEvent setOccurrenceParticipationStatus(const CalendarEvent& baseEvent,
                                                   const LocalDateTime& recurrenceId,
                                                   const std::string_view participantId,
                                                   std::string participationStatus)
    {
        auto result = baseEvent;
        auto& occurrence = result.recurrenceOverrides[recurrenceId.value];
        const auto participantOverride =
            occurrence.participantOverrides.find(std::string{participantId});
        if (participantOverride != occurrence.participantOverrides.end())
        {
            participantOverride->second.participationStatus = std::move(participationStatus);
            occurrence.participantParticipationStatus.erase(std::string{participantId});
            return result;
        }

        const auto baseParticipant =
            std::ranges::find(baseEvent.attendees, participantId, &Attendee::id);
        if (baseParticipant != baseEvent.attendees.end() &&
            baseParticipant->participationStatus == participationStatus)
            occurrence.participantParticipationStatus.erase(std::string{participantId});
        else
            occurrence.participantParticipationStatus.insert_or_assign(
                std::string{participantId}, std::move(participationStatus));

        if (!occurrence.start && !occurrence.duration && !occurrence.title &&
            occurrence.participantOverrides.empty() &&
            occurrence.participantParticipationStatus.empty())
            result.recurrenceOverrides.erase(recurrenceId.value);
        return result;
    }

    CalendarEvent excludeOccurrence(const CalendarEvent& baseEvent,
                                    const LocalDateTime& recurrenceId)
    {
        auto result = baseEvent;
        result.recurrenceOverrides[recurrenceId.value].excluded = true;
        return result;
    }

    CalendarEvent acknowledgeAlert(CalendarEvent event, Alert alert,
                                   const UtcInstant acknowledgedAt)
    {
        if (!event.useDefaultAlerts && !event.alerts.contains(alert.id))
            return event;
        alert.acknowledged = acknowledgedAt;
        event.alerts.insert_or_assign(alert.id, std::move(alert));
        return event;
    }
} // namespace javelin::jmap::calendar
