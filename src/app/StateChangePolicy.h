#pragma once

#include "app/WorkScheduler.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace javelin::app
{
    struct StateChangeCapabilities
    {
        bool calendar = false;
        bool contacts = false;
    };

    struct RoutedStateChanges
    {
        std::unordered_map<std::string, std::string> mailStates;
        bool calendarChanged = false;
        bool contactsChanged = false;
    };

    [[nodiscard]] inline std::vector<std::string>
    subscribedStateChangeTypes(const StateChangeCapabilities capabilities)
    {
        std::vector<std::string> types{"Email", "Mailbox"};
        if (capabilities.calendar)
        {
            types.emplace_back("Calendar");
            types.emplace_back("CalendarEvent");
        }
        if (capabilities.contacts)
        {
            types.emplace_back("AddressBook");
            types.emplace_back("ContactCard");
        }
        return types;
    }

    [[nodiscard]] inline bool shouldRestoreContactRefresh(const WorkStatus status)
    {
        return status == WorkStatus::Queued || status == WorkStatus::Paused ||
               status == WorkStatus::WaitingForNetwork || status == WorkStatus::WaitingForAuth;
    }

    [[nodiscard]] inline RoutedStateChanges
    routeStateChanges(std::unordered_map<std::string, std::string> changedStates)
    {
        RoutedStateChanges routed;
        for (auto& [type, state] : changedStates)
        {
            if (type == "Calendar" || type == "CalendarEvent")
            {
                routed.calendarChanged = true;
                continue;
            }
            if (type == "AddressBook" || type == "ContactCard")
            {
                routed.contactsChanged = true;
                continue;
            }
            routed.mailStates.insert_or_assign(std::move(type), std::move(state));
        }
        return routed;
    }

} // namespace javelin::app
