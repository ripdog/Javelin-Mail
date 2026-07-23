#include "jmap/sync/MailboxInterestRegistry.h"

#include <algorithm>
#include <utility>

namespace javelin::jmap::sync
{

    MailboxInterestRegistry::ObservationId MailboxInterestRegistry::observe(std::string accountId,
                                                                            std::string mailboxId)
    {
        const auto observationId = m_nextObservationId++;
        m_observations.emplace(observationId, Interest{.accountId = std::move(accountId),
                                                       .mailboxId = std::move(mailboxId)});
        return observationId;
    }

    std::optional<MailboxInterestRegistry::Interest>
    MailboxInterestRegistry::unobserve(const ObservationId observationId)
    {
        const auto found = m_observations.find(observationId);
        if (found == m_observations.end())
        {
            return std::nullopt;
        }

        auto interest = std::move(found->second);
        m_observations.erase(found);
        return interest;
    }

    std::vector<std::string>
    MailboxInterestRegistry::mailboxIds(const std::string_view accountId) const
    {
        std::vector<std::string> mailboxIds;
        for (const auto& [observationId, interest] : m_observations)
        {
            static_cast<void>(observationId);
            if (interest.accountId == accountId)
            {
                mailboxIds.push_back(interest.mailboxId);
            }
        }

        std::ranges::sort(mailboxIds);
        mailboxIds.erase(std::ranges::unique(mailboxIds).begin(), mailboxIds.end());
        return mailboxIds;
    }

    std::size_t MailboxInterestRegistry::observationCount(const std::string_view accountId,
                                                          const std::string_view mailboxId) const
    {
        return static_cast<std::size_t>(std::ranges::count_if(
            m_observations,
            [accountId, mailboxId](const auto& entry)
            {
                return entry.second.accountId == accountId && entry.second.mailboxId == mailboxId;
            }));
    }

    void
    MailboxInterestRegistry::eraseAccountsNotIn(const std::unordered_set<std::string>& accountIds)
    {
        std::erase_if(m_observations, [&accountIds](const auto& entry)
                      { return !accountIds.contains(entry.second.accountId); });
    }

} // namespace javelin::jmap::sync
