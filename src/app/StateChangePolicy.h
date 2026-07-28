#pragma once

#include "app/WorkScheduler.h"
#include "jmap/sync/StateChangeSource.h"

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
        javelin::jmap::sync::AccountTypeStateMap calendarStates;
        javelin::jmap::sync::AccountTypeStateMap contactStates;
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
    routeStateChanges(javelin::jmap::sync::AccountTypeStateMap changedStates,
                      const std::string_view primaryAccountId)
    {
        RoutedStateChanges routed;
        for (auto& [accountId, states] : changedStates)
        {
            for (auto& [type, state] : states)
            {
                if (type == "Calendar" || type == "CalendarEvent")
                {
                    routed.calendarStates[accountId].insert_or_assign(std::move(type),
                                                                      std::move(state));
                    continue;
                }
                if (type == "AddressBook" || type == "ContactCard")
                {
                    routed.contactStates[accountId].insert_or_assign(std::move(type),
                                                                     std::move(state));
                    continue;
                }
                if (accountId == primaryAccountId)
                    routed.mailStates.insert_or_assign(std::move(type), std::move(state));
            }
        }
        return routed;
    }

} // namespace javelin::app
