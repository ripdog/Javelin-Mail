#include "app/account/EndpointRetryGate.h"

#include <algorithm>

namespace javelin::app
{

    EndpointRetryGate::EndpointRetryGate(EndpointRetryPolicy policy, NowProvider now)
        : m_policy(policy), m_now(now ? std::move(now) : [] { return Clock::now(); })
    {
    }

    EndpointRetryDecision EndpointRetryGate::acquire(const std::string_view endpoint)
    {
        const auto found = m_states.find(std::string{endpoint});
        if (found == m_states.end())
            return {};

        auto& state = found->second;
        const auto now = m_now();
        if (now < state.notBefore)
        {
            auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(state.notBefore - now);
            if (remaining.count() == 0)
                remaining = std::chrono::milliseconds{1};
            return {.allowed = false, .probe = false, .retryAfter = remaining};
        }

        if (state.probeInFlight)
        {
            return {.allowed = false, .probe = false, .retryAfter = m_policy.probePollInterval};
        }

        state.probeInFlight = true;
        return {.allowed = true, .probe = true, .retryAfter = std::chrono::milliseconds{0}};
    }

    void EndpointRetryGate::recordFailure(const std::string_view endpoint,
                                          const std::optional<std::chrono::seconds> retryAfter)
    {
        const auto now = m_now();
        auto [found, inserted] = m_states.try_emplace(std::string{endpoint});
        auto& state = found->second;

        const bool failedRecoveryProbe = !inserted && state.probeInFlight;
        const bool startsNewAttempt = inserted || failedRecoveryProbe;
        if (inserted)
            state.consecutiveFailures = 1;
        else if (failedRecoveryProbe)
            ++state.consecutiveFailures;

        state.probeInFlight = false;
        if (startsNewAttempt)
        {
            auto delay = delayForAttempt(state.consecutiveFailures);
            if (retryAfter.has_value())
            {
                delay = std::max(
                    delay, std::chrono::duration_cast<std::chrono::milliseconds>(*retryAfter));
            }
            state.notBefore = now + delay;
            return;
        }

        if (retryAfter.has_value())
        {
            state.notBefore =
                std::max(state.notBefore,
                         now + std::chrono::duration_cast<std::chrono::milliseconds>(*retryAfter));
        }
    }

    void EndpointRetryGate::recordSuccess(const std::string_view endpoint)
    {
        m_states.erase(std::string{endpoint});
    }

    void EndpointRetryGate::releaseProbe(const std::string_view endpoint)
    {
        const auto found = m_states.find(std::string{endpoint});
        if (found != m_states.end())
            found->second.probeInFlight = false;
    }

    void EndpointRetryGate::reset(const std::string_view endpoint)
    {
        m_states.erase(std::string{endpoint});
    }

    std::chrono::milliseconds EndpointRetryGate::delayForAttempt(const std::size_t attempt) const
    {
        if (attempt == 0)
            return std::chrono::milliseconds{0};

        auto delay = m_policy.initialDelay;
        for (std::size_t current = 1; current < attempt; ++current)
            delay = std::min(delay * 2, m_policy.maxDelay);
        return std::min(delay, m_policy.maxDelay);
    }

} // namespace javelin::app
