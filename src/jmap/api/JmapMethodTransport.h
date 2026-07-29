#pragma once

#include "jmap/api/Cancellation.h"
#include "jmap/api/Error.h"
#include "jmap/api/MethodEnvelope.h"

#include <QCoroTask>

#include <QString>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace javelin::jmap::cache
{
    class DatabaseConnection;
}

namespace javelin::jmap::api
{
    class AbstractTransport;

    class WebSocketFailureCooldowns final
    {
      public:
        explicit WebSocketFailureCooldowns(
            std::chrono::milliseconds failureCooldown = std::chrono::minutes{15});

        [[nodiscard]] std::optional<std::chrono::milliseconds>
        retryDelay(std::string_view url) const;
        void recordFailure(std::string url);
        void recordSuccess(std::string_view url);

      private:
        std::chrono::milliseconds m_failureCooldown;
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_retryAfter;
    };

    enum class JmapTransportPolicy
    {
        Preferred,
        ForceWebSocket,
    };

    struct JmapMethodRequest
    {
        std::string accountId;
        std::string apiUrl;
        std::string accessToken;
        RequestEnvelope envelope;
        CancellationToken cancellation{};
        JmapTransportPolicy transportPolicy = JmapTransportPolicy::Preferred;
        std::function<void()> dispatched;
    };

    namespace detail
    {
        struct JmapRequestLogContext
        {
            QString methodCalls;
            QString mailboxes;
        };

        [[nodiscard]] JmapRequestLogContext
        describeJmapRequest(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                            const JmapMethodRequest& request);
    } // namespace detail

    using JmapMethodTransportResult = std::variant<ResponseEnvelope, TransportError, ProtocolError>;

    class JmapMethodTransport
    {
      public:
        virtual ~JmapMethodTransport() = default;

        virtual void invalidateConnection(std::string_view accountId);

        [[nodiscard]] virtual QCoro::Task<JmapMethodTransportResult>
        call(JmapMethodRequest request) = 0;
    };

    class HttpJmapMethodTransport final : public JmapMethodTransport
    {
      public:
        explicit HttpJmapMethodTransport(AbstractTransport& transport);

        void invalidateConnection(std::string_view accountId) override;

        [[nodiscard]] QCoro::Task<JmapMethodTransportResult>
        call(JmapMethodRequest request) override;

      private:
        AbstractTransport& m_transport;
    };

    class PreferredJmapMethodTransport final : public JmapMethodTransport
    {
      public:
        PreferredJmapMethodTransport(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                                     HttpJmapMethodTransport& httpTransport,
                                     WebSocketFailureCooldowns& cooldowns);
        ~PreferredJmapMethodTransport() override;

        PreferredJmapMethodTransport(const PreferredJmapMethodTransport&) = delete;
        PreferredJmapMethodTransport& operator=(const PreferredJmapMethodTransport&) = delete;
        PreferredJmapMethodTransport(PreferredJmapMethodTransport&&) = delete;
        PreferredJmapMethodTransport& operator=(PreferredJmapMethodTransport&&) = delete;

        void invalidateConnection(std::string_view accountId) override;

        [[nodiscard]] QCoro::Task<JmapMethodTransportResult>
        call(JmapMethodRequest request) override;

      private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

} // namespace javelin::jmap::api
