#pragma once

#include "jmap/api/Cancellation.h"
#include "jmap/api/Error.h"
#include "jmap/api/MethodEnvelope.h"

#include <QCoroTask>

#include <string>
#include <variant>

namespace javelin::jmap::api
{
    class AbstractTransport;

    struct JmapMethodRequest
    {
        std::string apiUrl;
        std::string accessToken;
        RequestEnvelope envelope;
        CancellationToken cancellation{};
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

} // namespace javelin::jmap::api
