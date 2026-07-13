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
            const auto address = trim(requested);
            const auto normalized = normalizedAddress(address);
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
            result.push_back({.id = std::move(id),
                              .name = address,
                              .email = address,
                              .calendarAddress = "mailto:" + address,
                              .participationStatus = "needs-action",
                              .isOwner = false,
                              .isAttendee = true,
                              .scheduleSequence = 0,
                              .scheduleUpdated = std::nullopt});
        }
        return result;
    }
} // namespace javelin::jmap::calendar
