#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace javelin::jmap::sync
{

    class MailboxInterestRegistry
    {
      public:
        using ObservationId = std::uint64_t;

        struct Interest
        {
            std::string accountId;
            std::string mailboxId;
        };

        [[nodiscard]] ObservationId observe(std::string accountId, std::string mailboxId);
        [[nodiscard]] std::optional<Interest> unobserve(ObservationId observationId);
        [[nodiscard]] std::vector<std::string> mailboxIds(std::string_view accountId) const;
        [[nodiscard]] std::size_t observationCount(std::string_view accountId,
                                                   std::string_view mailboxId) const;
        void eraseAccountsNotIn(const std::unordered_set<std::string>& accountIds);

      private:
        std::unordered_map<ObservationId, Interest> m_observations;
        ObservationId m_nextObservationId = 1;
    };

} // namespace javelin::jmap::sync
