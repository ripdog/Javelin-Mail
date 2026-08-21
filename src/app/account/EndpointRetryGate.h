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

        class Lease final
        {
          public:
            Lease() = default;
            Lease(const Lease&) = delete;
            Lease& operator=(const Lease&) = delete;
            Lease(Lease&& other) noexcept;
            Lease& operator=(Lease&& other) noexcept;
            ~Lease();

            [[nodiscard]] bool allowed() const;
            [[nodiscard]] bool probe() const;
            [[nodiscard]] std::chrono::milliseconds retryAfter() const;
            void recordFailure(std::optional<std::chrono::seconds> retryAfter = std::nullopt);
            void recordSuccess();

          private:
            friend class EndpointRetryGate;
            Lease(EndpointRetryGate* gate, std::string endpoint, EndpointRetryDecision decision);
            void release();

            EndpointRetryGate* m_gate = nullptr;
            std::string m_endpoint;
            EndpointRetryDecision m_decision;
        };

        explicit EndpointRetryGate(EndpointRetryPolicy policy = {}, NowProvider now = {});

        [[nodiscard]] Lease acquire(std::string_view endpoint);
        // Records a failure that is not owned by an acquired recovery probe. Such failures start
        // the first cooldown, or extend Retry-After, but never consume/advance another caller's
        // probe.
        void recordFailure(std::string_view endpoint,
                           std::optional<std::chrono::seconds> retryAfter = std::nullopt);
        void recordSuccess(std::string_view endpoint);
        void reset(std::string_view endpoint);

      private:
        struct State
        {
            std::size_t consecutiveFailures = 0;
            Clock::time_point notBefore{};
            bool probeInFlight = false;
        };

        [[nodiscard]] std::chrono::milliseconds delayForAttempt(std::size_t attempt) const;
        void recordLeaseFailure(std::string_view endpoint, bool ownsProbe,
                                std::optional<std::chrono::seconds> retryAfter);
        void releaseProbe(std::string_view endpoint);

        EndpointRetryPolicy m_policy;
        NowProvider m_now;
        std::unordered_map<std::string, State> m_states;
    };

} // namespace javelin::app
