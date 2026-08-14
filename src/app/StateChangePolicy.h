#pragma once

#include "app/WorkScheduler.h"
#include "jmap/sync/StateChangeSource.h"

#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace javelin::app
{
    struct StateChangeCapabilities
    {
        bool calendar = false;
        bool contacts = false;
        bool identities = false;
    };

    struct RoutedStateChanges
    {
        std::unordered_map<std::string, std::string> mailStates;
        javelin::jmap::sync::AccountTypeStateMap calendarStates;
        javelin::jmap::sync::AccountTypeStateMap contactStates;
        javelin::jmap::sync::AccountTypeStateMap identityStates;
    };

    [[nodiscard]] inline std::vector<std::string>
    subscribedStateChangeTypes(const StateChangeCapabilities capabilities)
    {
        std::vector<std::string> types{"Email", "Mailbox"};
        if (capabilities.identities)
            types.emplace_back("Identity");
        if (capabilities.calendar)
        {
            types.emplace_back("Calendar");
            types.emplace_back("CalendarEvent");
            types.emplace_back("CalendarEventNotification");
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

    [[nodiscard]] inline bool shouldDeferForActiveMutation(const std::string_view dataType)
    {
        return dataType != "AddressBook" && dataType != "ContactCard" &&
               dataType != "CalendarEventNotification";
    }

    [[nodiscard]] inline bool
    shouldRefreshMailboxWindow(const bool refreshEveryMailbox,
                               const std::span<const std::string> queryAffectedMailboxIds,
                               const std::span<const std::string> explicitlyRequestedMailboxIds,
                               const std::string_view mailboxId)
    {
        return refreshEveryMailbox ||
               std::ranges::find(queryAffectedMailboxIds, mailboxId) !=
                   queryAffectedMailboxIds.end() ||
               std::ranges::find(explicitlyRequestedMailboxIds, mailboxId) !=
                   explicitlyRequestedMailboxIds.end();
    }

    [[nodiscard]] inline bool
    shouldRefreshMailboxWindow(const bool refreshEveryMailbox,
                               const std::span<const std::string> queryAffectedMailboxIds,
                               const std::string_view mailboxId)
    {
        return shouldRefreshMailboxWindow(refreshEveryMailbox, queryAffectedMailboxIds,
                                          std::span<const std::string>{}, mailboxId);
    }

    [[nodiscard]] inline std::vector<std::string> newlyWatchedMailboxIds(
        const std::span<const std::pair<std::string, std::string>> previousMailboxes,
        const std::span<const std::pair<std::string, std::string>> updatedMailboxes)
    {
        std::vector<std::string> result;
        for (const auto& mailbox : updatedMailboxes)
        {
            if (std::ranges::none_of(previousMailboxes, [&mailbox](const auto& previousMailbox)
                                     { return previousMailbox.first == mailbox.first; }))
            {
                result.push_back(mailbox.first);
            }
        }
        return result;
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
                if (type == "Calendar" || type == "CalendarEvent" ||
                    type == "CalendarEventNotification")
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
                if (type == "Identity")
                {
                    routed.identityStates[accountId].insert_or_assign(std::move(type),
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
