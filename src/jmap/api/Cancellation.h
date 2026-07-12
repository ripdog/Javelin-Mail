#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace javelin::jmap::api
{
    namespace detail
    {
        struct CancellationState;
    }

    class CancellationRegistration
    {
      public:
        CancellationRegistration() = default;
        ~CancellationRegistration();

        CancellationRegistration(const CancellationRegistration&) = delete;
        CancellationRegistration& operator=(const CancellationRegistration&) = delete;
        CancellationRegistration(CancellationRegistration&& other) noexcept;
        CancellationRegistration& operator=(CancellationRegistration&& other) noexcept;

        void reset();

      private:
        friend class CancellationToken;

        CancellationRegistration(std::shared_ptr<detail::CancellationState> state,
                                 std::uint64_t callbackId);

        std::shared_ptr<detail::CancellationState> m_state;
        std::uint64_t m_callbackId = 0;
    };

    class CancellationToken
    {
      public:
        CancellationToken() = default;

        [[nodiscard]] bool isCancellationRequested() const;
        [[nodiscard]] CancellationRegistration
        registerCallback(std::function<void()> callback) const;

      private:
        friend class CancellationSource;

        explicit CancellationToken(std::shared_ptr<detail::CancellationState> state);

        std::shared_ptr<detail::CancellationState> m_state;
    };

    class CancellationSource
    {
      public:
        CancellationSource();

        [[nodiscard]] CancellationToken token() const;
        void cancel();

      private:
        std::shared_ptr<detail::CancellationState> m_state;
    };

} // namespace javelin::jmap::api
