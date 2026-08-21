#include "app/account/EndpointRetryGate.h"

#include <algorithm>
#include <utility>

namespace javelin::app
{

    EndpointRetryGate::Lease::Lease(EndpointRetryGate* gate, std::string endpoint,
                                    const EndpointRetryDecision decision)
        : m_gate(gate), m_endpoint(std::move(endpoint)), m_decision(decision)
    {
    }

    EndpointRetryGate::Lease::Lease(Lease&& other) noexcept
        : m_gate(std::exchange(other.m_gate, nullptr)), m_endpoint(std::move(other.m_endpoint)),
          m_decision(other.m_decision)
    {
        other.m_decision.probe = false;
    }

    EndpointRetryGate::Lease& EndpointRetryGate::Lease::operator=(Lease&& other) noexcept
    {
        if (this == &other)
            return *this;
        release();
        m_gate = std::exchange(other.m_gate, nullptr);
        m_endpoint = std::move(other.m_endpoint);
        m_decision = other.m_decision;
        other.m_decision.probe = false;
        return *this;
    }

    EndpointRetryGate::Lease::~Lease()
    {
        release();
    }

    bool EndpointRetryGate::Lease::allowed() const
    {
        return m_decision.allowed;
    }

    bool EndpointRetryGate::Lease::probe() const
    {
        return m_decision.probe;
    }

    std::chrono::milliseconds EndpointRetryGate::Lease::retryAfter() const
    {
        return m_decision.retryAfter;
    }

    void
    EndpointRetryGate::Lease::recordFailure(const std::optional<std::chrono::seconds> retryAfter)
    {
        if (m_gate == nullptr || !m_decision.allowed)
            return;
        m_gate->recordLeaseFailure(m_endpoint, m_decision.probe, retryAfter);
        m_decision.probe = false;
    }

    void EndpointRetryGate::Lease::recordSuccess()
    {
        if (m_gate == nullptr || !m_decision.allowed)
            return;
        m_gate->recordSuccess(m_endpoint);
        m_decision.probe = false;
    }

    void EndpointRetryGate::Lease::release()
    {
        if (m_gate != nullptr && m_decision.probe)
            m_gate->releaseProbe(m_endpoint);
        m_gate = nullptr;
        m_decision.probe = false;
    }

    EndpointRetryGate::EndpointRetryGate(EndpointRetryPolicy policy, NowProvider now)
        : m_policy(policy), m_now(now ? std::move(now) : [] { return Clock::now(); })
    {
    }

    EndpointRetryGate::Lease EndpointRetryGate::acquire(const std::string_view endpoint)
    {
        const std::string endpointKey{endpoint};
        const auto found = m_states.find(endpointKey);
        if (found == m_states.end())
            return Lease{this, endpointKey, {}};

        auto& state = found->second;
        const auto now = m_now();
        if (now < state.notBefore)
        {
            auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(state.notBefore - now);
            if (remaining.count() == 0)
                remaining = std::chrono::milliseconds{1};
            return Lease{nullptr, {}, {.allowed = false, .probe = false, .retryAfter = remaining}};
        }

        if (state.probeInFlight)
        {
            return Lease{
                nullptr,
                {},
                {.allowed = false, .probe = false, .retryAfter = m_policy.probePollInterval}};
        }

        state.probeInFlight = true;
        return Lease{this,
                     endpointKey,
                     {.allowed = true, .probe = true, .retryAfter = std::chrono::milliseconds{0}}};
    }

    void EndpointRetryGate::recordFailure(const std::string_view endpoint,
                                          const std::optional<std::chrono::seconds> retryAfter)
    {
        recordLeaseFailure(endpoint, false, retryAfter);
    }

    void EndpointRetryGate::recordLeaseFailure(const std::string_view endpoint,
                                               const bool ownsProbe,
                                               const std::optional<std::chrono::seconds> retryAfter)
    {
        const auto now = m_now();
        auto [found, inserted] = m_states.try_emplace(std::string{endpoint});
        auto& state = found->second;

        const bool failedRecoveryProbe = ownsProbe && !inserted && state.probeInFlight;
        const bool startsNewAttempt = inserted || failedRecoveryProbe;
        if (inserted)
            state.consecutiveFailures = 1;
        else if (failedRecoveryProbe)
            ++state.consecutiveFailures;

        if (failedRecoveryProbe)
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
