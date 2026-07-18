#include "jmap/calendar/CalendarEventEditing.h"

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
                              .scheduleSequence = 0,
                              .scheduleUpdated = std::nullopt});
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
        if (!occurrence.start && !occurrence.duration && !occurrence.title)
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
} // namespace javelin::jmap::calendar
