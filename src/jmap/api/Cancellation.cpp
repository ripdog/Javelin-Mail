#include "jmap/api/Cancellation.h"

#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace javelin::jmap::api
{
    namespace detail
    {
        struct CancellationState
        {
            std::mutex mutex;
            bool cancellationRequested = false;
            std::uint64_t nextCallbackId = 1;
            std::unordered_map<std::uint64_t, std::function<void()>> callbacks;
        };
    } // namespace detail

    CancellationRegistration::CancellationRegistration(
        std::shared_ptr<detail::CancellationState> state, const std::uint64_t callbackId)
        : m_state(std::move(state)), m_callbackId(callbackId)
    {
    }

    CancellationRegistration::~CancellationRegistration()
    {
        reset();
    }

    CancellationRegistration::CancellationRegistration(CancellationRegistration&& other) noexcept
        : m_state(std::move(other.m_state)), m_callbackId(std::exchange(other.m_callbackId, 0))
    {
    }

    CancellationRegistration&
    CancellationRegistration::operator=(CancellationRegistration&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }

        reset();
        m_state = std::move(other.m_state);
        m_callbackId = std::exchange(other.m_callbackId, 0);
        return *this;
    }

    void CancellationRegistration::reset()
    {
        if (m_state == nullptr || m_callbackId == 0)
        {
            return;
        }

        const std::scoped_lock lock{m_state->mutex};
        m_state->callbacks.erase(m_callbackId);
        m_callbackId = 0;
        m_state.reset();
    }

    CancellationToken::CancellationToken(std::shared_ptr<detail::CancellationState> state)
        : m_state(std::move(state))
    {
    }

    bool CancellationToken::isCancellationRequested() const
    {
        if (m_state == nullptr)
        {
            return false;
        }

        const std::scoped_lock lock{m_state->mutex};
        return m_state->cancellationRequested;
    }

    CancellationRegistration
    CancellationToken::registerCallback(std::function<void()> callback) const
    {
        if (m_state == nullptr)
        {
            return {};
        }

        std::uint64_t callbackId = 0;
        bool invokeImmediately = false;
        {
            const std::scoped_lock lock{m_state->mutex};
            if (m_state->cancellationRequested)
            {
                invokeImmediately = true;
            }
            else
            {
                callbackId = m_state->nextCallbackId++;
                m_state->callbacks.emplace(callbackId, callback);
            }
        }

        if (invokeImmediately)
        {
            callback();
            return {};
        }

        return CancellationRegistration{m_state, callbackId};
    }

    CancellationSource::CancellationSource()
        : m_state(std::make_shared<detail::CancellationState>())
    {
    }

    CancellationToken CancellationSource::token() const
    {
        return CancellationToken{m_state};
    }

    void CancellationSource::cancel()
    {
        std::vector<std::function<void()>> callbacks;
        {
            const std::scoped_lock lock{m_state->mutex};
            if (m_state->cancellationRequested)
            {
                return;
            }

            m_state->cancellationRequested = true;
            callbacks.reserve(m_state->callbacks.size());
            for (auto& [callbackId, callback] : m_state->callbacks)
            {
                static_cast<void>(callbackId);
                callbacks.push_back(std::move(callback));
            }
            m_state->callbacks.clear();
        }

        for (const auto& callback : callbacks)
        {
            callback();
        }
    }

} // namespace javelin::jmap::api
