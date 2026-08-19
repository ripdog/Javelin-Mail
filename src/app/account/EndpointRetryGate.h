#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace javelin::app
{

    struct EndpointRetryPolicy
    {
        std::chrono::milliseconds initialDelay{1000};
        std::chrono::milliseconds maxDelay{60000};
        std::chrono::milliseconds probePollInterval{250};
    };

    struct EndpointRetryDecision
    {
        bool allowed = true;
        bool probe = false;
        std::chrono::milliseconds retryAfter{0};
    };

    class EndpointRetryGate final
    {
      public:
        using Clock = std::chrono::steady_clock;
        using NowProvider = std::function<Clock::time_point()>;

        explicit EndpointRetryGate(EndpointRetryPolicy policy = {}, NowProvider now = {});

        [[nodiscard]] EndpointRetryDecision acquire(std::string_view endpoint);
        void recordFailure(std::string_view endpoint,
                           std::optional<std::chrono::seconds> retryAfter = std::nullopt);
        void recordSuccess(std::string_view endpoint);
        void releaseProbe(std::string_view endpoint);
        void reset(std::string_view endpoint);

      private:
        struct State
        {
            std::size_t consecutiveFailures = 0;
            Clock::time_point notBefore{};
            bool probeInFlight = false;
        };

        [[nodiscard]] std::chrono::milliseconds delayForAttempt(std::size_t attempt) const;

        EndpointRetryPolicy m_policy;
        NowProvider m_now;
        std::unordered_map<std::string, State> m_states;
    };

} // namespace javelin::app
