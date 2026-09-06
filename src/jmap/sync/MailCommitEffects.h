#pragma once

#include "jmap/domain/MailEntities.h"

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_set>
#include <vector>

namespace javelin::jmap::sync
{
    struct MailCommitEffects
    {
        bool emailObjectsChanged = false;
        bool mailboxMembershipChanged = false;
        bool sourceIdentityChanged = false;
        std::vector<std::string> affectedMailboxIds;
    };

    inline void appendAffectedMailboxId(MailCommitEffects& effects, const std::string& mailboxId)
    {
        if (std::ranges::find(effects.affectedMailboxIds, mailboxId) ==
            effects.affectedMailboxIds.end())
        {
            effects.affectedMailboxIds.push_back(mailboxId);
        }
    }

    inline bool sameMailboxMembership(const std::vector<std::string>& left,
                                      const std::vector<std::string>& right)
    {
        if (left.size() != right.size())
            return false;
        const std::unordered_set<std::string> leftSet(left.begin(), left.end());
        return std::ranges::all_of(right, [&leftSet](const std::string& id)
                                   { return leftSet.contains(id); });
    }

    inline void
    accumulateMailCommitEffects(MailCommitEffects& effects,
                                const std::optional<javelin::jmap::domain::Email>& previous,
                                const javelin::jmap::domain::Email& current)
    {
        effects.emailObjectsChanged =
            effects.emailObjectsChanged || !previous.has_value() || *previous != current;
        const bool membershipChanged =
            !previous.has_value() ||
            !sameMailboxMembership(previous->mailboxIds, current.mailboxIds);
        const bool sourceIdentityChanged =
            !previous.has_value() || previous->blobId != current.blobId;
        effects.mailboxMembershipChanged = effects.mailboxMembershipChanged || membershipChanged;
        effects.sourceIdentityChanged = effects.sourceIdentityChanged || sourceIdentityChanged;
        if (!membershipChanged && !sourceIdentityChanged)
            return;

        if (previous.has_value())
        {
            for (const auto& mailboxId : previous->mailboxIds)
                appendAffectedMailboxId(effects, mailboxId);
        }
        for (const auto& mailboxId : current.mailboxIds)
            appendAffectedMailboxId(effects, mailboxId);
    }

    inline void
    accumulateMailRemovalEffects(MailCommitEffects& effects,
                                 const std::optional<javelin::jmap::domain::Email>& previous)
    {
        if (!previous.has_value())
            return;
        effects.emailObjectsChanged = true;
        effects.mailboxMembershipChanged = true;
        effects.sourceIdentityChanged = true;
        for (const auto& mailboxId : previous->mailboxIds)
            appendAffectedMailboxId(effects, mailboxId);
    }

    inline void mergeMailCommitEffects(MailCommitEffects& target, const MailCommitEffects& source)
    {
        target.emailObjectsChanged = target.emailObjectsChanged || source.emailObjectsChanged;
        target.mailboxMembershipChanged =
            target.mailboxMembershipChanged || source.mailboxMembershipChanged;
        target.sourceIdentityChanged = target.sourceIdentityChanged || source.sourceIdentityChanged;
        for (const auto& mailboxId : source.affectedMailboxIds)
            appendAffectedMailboxId(target, mailboxId);
    }
} // namespace javelin::jmap::sync
