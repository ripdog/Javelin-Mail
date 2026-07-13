#pragma once

#include "jmap/api/Cancellation.h"
#include "jmap/api/Error.h"
#include "jmap/api/MethodEnvelope.h"

#include <QCoroTask>

#include <memory>
#include <string>
#include <variant>

namespace javelin::jmap::cache
{
    class DatabaseConnection;
}

namespace javelin::jmap::api
{
    class AbstractTransport;

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
    };

    using JmapMethodTransportResult = std::variant<ResponseEnvelope, TransportError, ProtocolError>;

    class JmapMethodTransport
    {
      public:
        virtual ~JmapMethodTransport() = default;

        [[nodiscard]] virtual QCoro::Task<JmapMethodTransportResult>
        call(JmapMethodRequest request) = 0;
    };

    class HttpJmapMethodTransport final : public JmapMethodTransport
    {
      public:
        explicit HttpJmapMethodTransport(AbstractTransport& transport);

        [[nodiscard]] QCoro::Task<JmapMethodTransportResult>
        call(JmapMethodRequest request) override;

      private:
        AbstractTransport& m_transport;
    };

    class PreferredJmapMethodTransport final : public JmapMethodTransport
    {
      public:
        PreferredJmapMethodTransport(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                                     HttpJmapMethodTransport& httpTransport);
        ~PreferredJmapMethodTransport() override;

        PreferredJmapMethodTransport(const PreferredJmapMethodTransport&) = delete;
        PreferredJmapMethodTransport& operator=(const PreferredJmapMethodTransport&) = delete;
        PreferredJmapMethodTransport(PreferredJmapMethodTransport&&) = delete;
        PreferredJmapMethodTransport& operator=(PreferredJmapMethodTransport&&) = delete;

        [[nodiscard]] QCoro::Task<JmapMethodTransportResult>
        call(JmapMethodRequest request) override;

      private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

} // namespace javelin::jmap::api
