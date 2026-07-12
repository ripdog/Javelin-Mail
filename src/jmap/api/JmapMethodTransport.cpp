#include "jmap/api/JmapMethodTransport.h"

#include "jmap/api/Transport.h"

#include <QByteArray>
#include <QUrl>

namespace javelin::jmap::api
{
    HttpJmapMethodTransport::HttpJmapMethodTransport(AbstractTransport& transport)
        : m_transport(transport)
    {
    }

    QCoro::Task<JmapMethodTransportResult> HttpJmapMethodTransport::call(JmapMethodRequest request)
    {
        const auto body = serializeRequestEnvelope(request.envelope);
        if (!body.has_value())
        {
            co_return ProtocolError{
                .code = ProtocolErrorCode::InvalidResponse,
                .message = "Failed to serialize JMAP request envelope",
            };
        }

        const auto result = co_await m_transport.send(HttpRequest{
            .method = HttpMethod::Post,
            .url = QUrl{QString::fromStdString(request.apiUrl)},
            .headers =
                {
                    HttpHeader{.name = "Authorization",
                               .value = QByteArray{"Bearer "} +
                                        QByteArray::fromStdString(request.accessToken)},
                    HttpHeader{.name = "Accept", .value = "application/json"},
                    HttpHeader{.name = "Content-Type", .value = "application/json"},
                },
            .body = QByteArray::fromStdString(*body),
        });
        if (const auto* error = std::get_if<TransportError>(&result))
        {
            co_return *error;
        }

        const auto parsed =
            parseResponseEnvelope(std::get<HttpResponse>(result).body.toStdString());
        if (!parsed.ok())
        {
            co_return ProtocolError{
                .code = ProtocolErrorCode::InvalidResponse,
                .message = *parsed.error,
            };
        }
        co_return *parsed.value;
    }

} // namespace javelin::jmap::api
