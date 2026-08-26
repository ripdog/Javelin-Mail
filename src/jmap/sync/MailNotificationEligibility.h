#pragma once

#include "jmap/domain/MailEntities.h"

#include <algorithm>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace javelin::jmap::sync
{
    struct MailNotificationTransition
    {
        const javelin::jmap::domain::Email* previous = nullptr;
        const javelin::jmap::domain::Email* current = nullptr;
        std::span<const std::string> notificationMailboxIds;
        bool serverCreated = false;
        bool withinNotificationHorizon = false;
        bool suppressedByLocalOperation = false;
    };

    struct MailNotificationEligibility
    {
        std::vector<std::string> qualifyingMailboxIds;

        [[nodiscard]] bool eligible() const
        {
            return !qualifyingMailboxIds.empty();
        }
    };

    [[nodiscard]] inline bool hasKeyword(const javelin::jmap::domain::Email& email,
                                         const std::string_view keyword)
    {
        return std::ranges::find(email.keywords, keyword) != email.keywords.end();
    }

    [[nodiscard]] inline std::vector<std::string>
    notificationMailboxIntersection(const javelin::jmap::domain::Email& email,
                                    const std::span<const std::string> notificationMailboxIds)
    {
        std::vector<std::string> result;
        for (const auto& mailboxId : notificationMailboxIds)
        {
            if (std::ranges::find(email.mailboxIds, mailboxId) != email.mailboxIds.end())
                result.push_back(mailboxId);
        }
        std::ranges::sort(result);
        result.erase(std::ranges::unique(result).begin(), result.end());
        return result;
    }

    [[nodiscard]] inline MailNotificationEligibility
    evaluateMailNotificationTransition(const MailNotificationTransition& transition)
    {
        if (transition.current == nullptr || !transition.withinNotificationHorizon ||
            transition.suppressedByLocalOperation || hasKeyword(*transition.current, "$seen"))
        {
            return {};
        }

        auto currentMailboxes =
            notificationMailboxIntersection(*transition.current, transition.notificationMailboxIds);
        if (currentMailboxes.empty())
            return {};

        if (transition.serverCreated)
            return {.qualifyingMailboxIds = std::move(currentMailboxes)};

        if (transition.previous == nullptr)
            return {};

        if (!notificationMailboxIntersection(*transition.previous,
                                             transition.notificationMailboxIds)
                 .empty())
        {
            return {};
        }

        return {.qualifyingMailboxIds = std::move(currentMailboxes)};
    }

} // namespace javelin::jmap::sync
