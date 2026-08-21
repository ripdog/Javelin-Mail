#include "jmap/api/JmapMethodTransport.h"

#include "jmap/api/Transport.h"
#include "jmap/cache/JmapTransportPreferenceRepository.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QCoroSignal>

#include <QByteArray>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QNetworkRequest>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketHandshakeOptions>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace javelin::jmap::api::detail
{
    struct WebSocketMessageHeader
    {
        std::optional<std::string> type;
        std::optional<std::string> requestId;
        std::optional<std::string> title;
        std::optional<std::string> detail;
        std::optional<int> status;
    };
} // namespace javelin::jmap::api::detail

template <> struct glz::meta<javelin::jmap::api::detail::WebSocketMessageHeader>
{
    using T = javelin::jmap::api::detail::WebSocketMessageHeader;
    static constexpr auto value =
        glz::object("@type", &T::type, "requestId", &T::requestId, "title", &T::title, "detail",
                    &T::detail, "status", &T::status);
};

namespace javelin::jmap::api::detail
{
    namespace
    {
        struct MailboxReference
        {
            std::string accountId;
            std::string mailboxId;
        };

        void appendMailboxReference(std::vector<MailboxReference>& references,
                                    std::string accountId, std::string mailboxId)
        {
            if (mailboxId.empty())
                return;
            const auto duplicate = std::ranges::find_if(
                references, [&accountId, &mailboxId](const MailboxReference& reference)
                { return reference.accountId == accountId && reference.mailboxId == mailboxId; });
            if (duplicate == references.end())
            {
                references.push_back(MailboxReference{
                    .accountId = std::move(accountId),
                    .mailboxId = std::move(mailboxId),
                });
            }
        }

        void replaceAll(std::string& value, const std::string_view from, const std::string_view to)
        {
            std::size_t position = 0;
            while ((position = value.find(from, position)) != std::string::npos)
            {
                value.replace(position, from.size(), to);
                position += to.size();
            }
        }

        [[nodiscard]] std::string decodeJsonPointerToken(std::string token)
        {
            replaceAll(token, "~1", "/");
            replaceAll(token, "~0", "~");
            return token;
        }

        void collectMailboxReferences(const glz::generic& value, const std::string& accountId,
                                      std::vector<MailboxReference>& references)
        {
            if (value.is_array())
            {
                for (const auto& item : value.get_array())
                    collectMailboxReferences(item, accountId, references);
                return;
            }
            if (!value.is_object())
                return;

            for (const auto& [key, child] : value.get_object())
            {
                if ((key == "inMailbox" || key == "mailboxId") && child.is_string())
                {
                    appendMailboxReference(references, accountId, child.get_string());
                }
                else if (key == "inMailboxOtherThan" && child.is_array())
                {
                    for (const auto& mailbox : child.get_array())
                    {
                        if (mailbox.is_string())
                            appendMailboxReference(references, accountId, mailbox.get_string());
                    }
                }
                else if (key == "mailboxIds")
                {
                    if (child.is_object())
                    {
                        for (const auto& [mailboxId, enabled] : child.get_object())
                        {
                            static_cast<void>(enabled);
                            appendMailboxReference(references, accountId, mailboxId);
                        }
                    }
                    else if (child.is_array())
                    {
                        for (const auto& mailbox : child.get_array())
                        {
                            if (mailbox.is_string())
                                appendMailboxReference(references, accountId, mailbox.get_string());
                        }
                    }
                }
                else if (key.starts_with("mailboxIds/"))
                {
                    appendMailboxReference(references, accountId,
                                           decodeJsonPointerToken(key.substr(11)));
                }
                collectMailboxReferences(child, accountId, references);
            }
        }

        void collectMailboxMethodReferences(const MethodInvocation& invocation,
                                            const glz::generic& arguments,
                                            const std::string& accountId,
                                            std::vector<MailboxReference>& references)
        {
            if (invocation.name == "Mailbox/get" && arguments.contains("ids") &&
                arguments.at("ids").is_array())
            {
                for (const auto& mailbox : arguments.at("ids").get_array())
                {
                    if (mailbox.is_string())
                        appendMailboxReference(references, accountId, mailbox.get_string());
                }
            }
            else if (invocation.name == "Mailbox/set")
            {
                if (arguments.contains("update") && arguments.at("update").is_object())
                {
                    for (const auto& [mailboxId, patch] : arguments.at("update").get_object())
                    {
                        static_cast<void>(patch);
                        appendMailboxReference(references, accountId, mailboxId);
                    }
                }
                if (arguments.contains("destroy") && arguments.at("destroy").is_array())
                {
                    for (const auto& mailbox : arguments.at("destroy").get_array())
                    {
                        if (mailbox.is_string())
                            appendMailboxReference(references, accountId, mailbox.get_string());
                    }
                }
            }
        }

        struct RequestDescription
        {
            QStringList methodCalls;
            std::vector<MailboxReference> mailboxReferences;
        };

        [[nodiscard]] RequestDescription collectRequestDescription(const JmapMethodRequest& request)
        {
            RequestDescription description;
            for (const auto& invocation : request.envelope.methodCalls)
            {
                QString method = QString::fromStdString(invocation.name);
                if (!invocation.callId.empty())
                {
                    method += QStringLiteral("(") + QString::fromStdString(invocation.callId) +
                              QStringLiteral(")");
                }
                description.methodCalls.push_back(std::move(method));

                glz::generic arguments;
                auto json = invocation.arguments;
                if (glz::read_json(arguments, json) || !arguments.is_object())
                    continue;
                std::string accountId = request.accountId;
                if (arguments.contains("accountId") && arguments.at("accountId").is_string())
                    accountId = arguments.at("accountId").get_string();
                collectMailboxReferences(arguments, accountId, description.mailboxReferences);
                collectMailboxMethodReferences(invocation, arguments, accountId,
                                               description.mailboxReferences);
            }
            return description;
        }

        [[nodiscard]] QStringList mailboxIdLabels(const std::vector<MailboxReference>& references)
        {
            QStringList labels;
            labels.reserve(static_cast<qsizetype>(references.size()));
            for (const auto& reference : references)
                labels.push_back(
                    QStringLiteral("[%1]").arg(QString::fromStdString(reference.mailboxId)));
            return labels;
        }

        [[nodiscard]] QString boundedDiagnosticText(const std::string_view value,
                                                    const qsizetype maximumLength = 180)
        {
            if (maximumLength <= 0)
                return {};
            constexpr std::size_t maximumSourceBytes = 512;
            const auto sourceBytes = std::min(value.size(), maximumSourceBytes);
            auto text =
                QString::fromUtf8(value.data(), static_cast<qsizetype>(sourceBytes)).simplified();
            const bool truncated = value.size() > sourceBytes || text.size() > maximumLength;
            if (text.size() > maximumLength)
                text = text.left(maximumLength);
            if (truncated)
            {
                if (text.size() >= maximumLength)
                    text = text.left(maximumLength - 1);
                text += QChar{0x2026};
            }
            return text;
        }

        [[nodiscard]] QString diagnosticText(const glz::generic& value, const std::string_view key)
        {
            if (!value.is_object() || !value.contains(key) || !value.at(key).is_string())
                return {};
            return boundedDiagnosticText(value.at(key).get_string());
        }
    } // namespace

    JmapRequestLogContext describeJmapRequest(const JmapMethodRequest& request)
    {
        const auto description = collectRequestDescription(request);
        return JmapRequestLogContext{
            .methodCalls = description.methodCalls.join(QStringLiteral(", ")),
            .mailboxes = mailboxIdLabels(description.mailboxReferences).join(QStringLiteral(", ")),
        };
    }

    JmapRequestLogContext
    describeJmapRequest(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                        const JmapMethodRequest& request)
    {
        const auto description = collectRequestDescription(request);
        QStringList mailboxLabels;
        mailboxLabels.reserve(static_cast<qsizetype>(description.mailboxReferences.size()));
        QSqlQuery mailboxQuery{databaseConnection.database()};
        mailboxQuery.prepare(QStringLiteral(
            "SELECT name FROM mailboxes WHERE account_id=:account AND mailbox_id=:mailbox"));
        for (const auto& reference : description.mailboxReferences)
        {
            const QString mailboxId = QString::fromStdString(reference.mailboxId);
            mailboxQuery.bindValue(QStringLiteral(":account"),
                                   QString::fromStdString(reference.accountId));
            mailboxQuery.bindValue(QStringLiteral(":mailbox"), mailboxId);
            QString name;
            if (mailboxQuery.exec() && mailboxQuery.next())
                name = mailboxQuery.value(0).toString();
            mailboxQuery.finish();
            mailboxLabels.push_back(name.isEmpty()
                                        ? QStringLiteral("[%1]").arg(mailboxId)
                                        : QStringLiteral("%1 [%2]").arg(name, mailboxId));
        }

        return JmapRequestLogContext{
            .methodCalls = description.methodCalls.join(QStringLiteral(", ")),
            .mailboxes = mailboxLabels.join(QStringLiteral(", ")),
        };
    }

    QString describeWebSocketFrame(const std::string_view payload)
    {
        constexpr std::size_t maximumDiagnosticParseBytes = 64 * 1024;
        if (payload.size() > maximumDiagnosticParseBytes)
            return QStringLiteral("bytes=%1 parse=skipped-large").arg(payload.size());

        glz::generic frame;
        auto json = std::string{payload};
        if (glz::read_json(frame, json) || !frame.is_object())
            return QStringLiteral("bytes=%1 parse=invalid").arg(payload.size());

        QStringList parts;
        parts.push_back(QStringLiteral("bytes=%1").arg(payload.size()));

        constexpr std::size_t maximumDiagnosticElements = 24;
        QStringList keys;
        const auto& object = frame.get_object();
        std::size_t keyCount = 0;
        for (const auto& [key, value] : object)
        {
            static_cast<void>(value);
            if (keyCount++ >= maximumDiagnosticElements)
                break;
            keys.push_back(boundedDiagnosticText(key, 80));
        }
        keys.sort(Qt::CaseSensitive);
        if (object.size() > maximumDiagnosticElements)
            keys.push_back(QStringLiteral("…(+%1)").arg(object.size() - maximumDiagnosticElements));
        parts.push_back(QStringLiteral("keys=%1").arg(keys.join(QStringLiteral(","))));

        const auto type = diagnosticText(frame, "@type");
        parts.push_back(
            QStringLiteral("type=%1").arg(type.isEmpty() ? QStringLiteral("<missing>") : type));

        const auto requestId = diagnosticText(frame, "requestId");
        parts.push_back(QStringLiteral("requestId=%1")
                            .arg(requestId.isEmpty() ? QStringLiteral("<missing>") : requestId));

        if (frame.contains("methodResponses") && frame.at("methodResponses").is_array())
        {
            QStringList responses;
            QStringList errors;
            const auto& methodResponses = frame.at("methodResponses").get_array();
            std::size_t responseCount = 0;
            for (const auto& invocation : methodResponses)
            {
                if (responseCount++ >= maximumDiagnosticElements)
                    break;
                if (!invocation.is_array())
                    continue;
                const auto& fields = invocation.get_array();
                if (fields.size() < 3 || !fields[0].is_string() || !fields[2].is_string())
                    continue;

                const auto method = boundedDiagnosticText(fields[0].get_string(), 80);
                const auto callId = boundedDiagnosticText(fields[2].get_string(), 80);
                responses.push_back(QStringLiteral("%1(%2)").arg(method, callId));

                if (fields[0].get_string() == "error" && fields[1].is_object())
                {
                    const auto errorType = diagnosticText(fields[1], "type");
                    const auto description = diagnosticText(fields[1], "description");
                    QString error = QStringLiteral("%1:%2").arg(
                        callId, errorType.isEmpty() ? QStringLiteral("<unknown>") : errorType);
                    if (!description.isEmpty())
                        error += QStringLiteral("[%1]").arg(description);
                    errors.push_back(std::move(error));
                }
            }
            if (methodResponses.size() > maximumDiagnosticElements)
                responses.push_back(QStringLiteral("…(+%1)").arg(methodResponses.size() -
                                                                 maximumDiagnosticElements));
            if (!responses.isEmpty())
                parts.push_back(
                    QStringLiteral("methods=%1").arg(responses.join(QStringLiteral(","))));
            if (!errors.isEmpty())
                parts.push_back(QStringLiteral("errors=%1").arg(errors.join(QStringLiteral(","))));
        }

        const auto title = diagnosticText(frame, "title");
        const auto detail = diagnosticText(frame, "detail");
        if (!title.isEmpty())
            parts.push_back(QStringLiteral("title=%1").arg(title));
        if (!detail.isEmpty())
            parts.push_back(QStringLiteral("detail=%1").arg(detail));

        return parts.join(QStringLiteral(" "));
    }
} // namespace javelin::jmap::api::detail

namespace javelin::jmap::api
{
    Q_LOGGING_CATEGORY(logJmapMethodTransport, "jmap.transport.method")
    Q_LOGGING_CATEGORY(logJmapWebSocketTransport, "jmap.transport.websocket")

    namespace
    {
        constexpr auto connectTimeout = std::chrono::seconds{15};
        constexpr auto responseTimeout = std::chrono::seconds{60};
        constexpr auto cancellationPollInterval = std::chrono::milliseconds{250};

        using WebSocketMessageHeader = detail::WebSocketMessageHeader;

        [[nodiscard]] QString requestContextText(const detail::JmapRequestLogContext& context)
        {
            QString text = QStringLiteral("methods %1").arg(context.methodCalls);
            if (!context.mailboxes.isEmpty())
                text += QStringLiteral("; mailboxes %1").arg(context.mailboxes);
            return text;
        }

        [[nodiscard]] bool hasMethodErrors(const ResponseEnvelope& response)
        {
            return std::ranges::any_of(response.methodResponses,
                                       [](const MethodInvocation& invocation)
                                       { return invocation.name == "error"; });
        }

        [[nodiscard]] QString resultText(const JmapMethodTransportResult& result)
        {
            if (const auto* response = std::get_if<ResponseEnvelope>(&result))
            {
                if (!hasMethodErrors(*response))
                    return QStringLiteral("success");
                const auto errorCount = std::ranges::count_if(
                    response->methodResponses,
                    [](const MethodInvocation& invocation) { return invocation.name == "error"; });
                return QStringLiteral("failure: %1 JMAP method error response(s)").arg(errorCount);
            }
            if (const auto* error = std::get_if<TransportError>(&result))
            {
                const QString status = error->code == TransportErrorCode::Cancelled
                                           ? QStringLiteral("cancelled")
                                           : QStringLiteral("failure");
                return QStringLiteral("%1: %2: %3")
                    .arg(status, QString::fromStdString(std::string{toString(error->code)}),
                         QString::fromStdString(error->message));
            }
            const auto& error = std::get<ProtocolError>(result);
            return QStringLiteral("failure: %1: %2")
                .arg(QString::fromStdString(std::string{toString(error.code)}),
                     QString::fromStdString(error.message));
        }

        [[nodiscard]] bool isSuccessfulResult(const JmapMethodTransportResult& result)
        {
            const auto* response = std::get_if<ResponseEnvelope>(&result);
            return response != nullptr && !hasMethodErrors(*response);
        }

        [[nodiscard]] bool isCancelledResult(const JmapMethodTransportResult& result)
        {
            const auto* error = std::get_if<TransportError>(&result);
            return error != nullptr && error->code == TransportErrorCode::Cancelled;
        }

        [[nodiscard]] std::string nextMethodRequestId()
        {
            static std::atomic_uint64_t nextRequestId = 0;
            return "javelin-" + std::to_string(++nextRequestId);
        }

        class MethodRequestTrace final
        {
          public:
            MethodRequestTrace(QString requestId, QString transport,
                               const detail::JmapRequestLogContext& context)
                : m_requestId(std::move(requestId)), m_transport(std::move(transport)),
                  m_contextText(requestContextText(context))
            {
                m_elapsed.start();
                qCDebug(logJmapMethodTransport).noquote()
                    << "request started" << m_requestId << m_transport << m_contextText;
            }

            [[nodiscard]] JmapMethodTransportResult finish(JmapMethodTransportResult result) const
            {
                const QString outcome = resultText(result);
                if (isSuccessfulResult(result) || isCancelledResult(result))
                {
                    qCDebug(logJmapMethodTransport).noquote()
                        << "request finished" << m_requestId << m_transport << outcome
                        << m_elapsed.elapsed() << "ms" << m_contextText;
                }
                else
                {
                    qCWarning(logJmapMethodTransport).noquote()
                        << "request finished" << m_requestId << m_transport << outcome
                        << m_elapsed.elapsed() << "ms" << m_contextText;
                }
                return result;
            }

          private:
            QString m_requestId;
            QString m_transport;
            QString m_contextText;
            QElapsedTimer m_elapsed;
        };

        struct BufferedWebSocketMessage
        {
            std::string type;
            std::string payload;
        };

        struct WebSocketAttemptResult
        {
            JmapMethodTransportResult result;
            bool requestDispatched = false;
            bool validWebSocketResponse = false;
        };

        [[nodiscard]] TransportError cancelledError(const std::string_view message)
        {
            return TransportError{
                .code = TransportErrorCode::Cancelled,
                .message = std::string{message},
                .httpStatus = std::nullopt,
            };
        }

        [[nodiscard]] TransportError networkError(const std::string_view message)
        {
            return TransportError{
                .code = TransportErrorCode::NetworkFailure,
                .message = std::string{message},
                .httpStatus = std::nullopt,
            };
        }

        [[nodiscard]] TransportError decodingError(const std::string_view message)
        {
            return TransportError{
                .code = TransportErrorCode::ResponseDecodingFailed,
                .message = std::string{message},
                .httpStatus = std::nullopt,
            };
        }

        [[nodiscard]] ProtocolError invalidRequestError(const std::string_view message)
        {
            return ProtocolError{
                .code = ProtocolErrorCode::InvalidRequest,
                .message = std::string{message},
            };
        }

        [[nodiscard]] ProtocolError invalidResponseError(const std::string_view message)
        {
            return ProtocolError{
                .code = ProtocolErrorCode::InvalidResponse,
                .message = std::string{message},
            };
        }

        [[nodiscard]] QByteArray bearerAuthorizationValue(const std::string_view accessToken)
        {
            return QByteArrayLiteral("Bearer ") +
                   QByteArray::fromStdString(std::string{accessToken});
        }

        [[nodiscard]] QString requestErrorMessage(const WebSocketMessageHeader& header)
        {
            QString message = QStringLiteral("The JMAP WebSocket server rejected the request.");
            if (header.title.has_value() && !header.title->empty())
            {
                message = QString::fromStdString(*header.title);
            }
            if (header.detail.has_value() && !header.detail->empty())
            {
                message += QStringLiteral(": ") + QString::fromStdString(*header.detail);
            }
            if (header.status.has_value())
            {
                message += QStringLiteral(" (status %1)").arg(*header.status);
            }
            return message;
        }

        class WebSocketJmapConnection final
        {
          public:
            WebSocketJmapConnection()
            {
                m_pingTimer.setInterval(std::chrono::seconds{30});
                QObject::connect(&m_pingTimer, &QTimer::timeout, &m_socket,
                                 [this]()
                                 {
                                     if (m_socket.state() == QAbstractSocket::ConnectedState)
                                     {
                                         m_socket.ping();
                                     }
                                 });
                QObject::connect(&m_socket, &QWebSocket::connected, &m_socket,
                                 [this]()
                                 {
                                     m_opening = false;
                                     m_pingTimer.start();
                                     qCInfo(logJmapWebSocketTransport) << "connected";
                                 });
                QObject::connect(&m_socket, &QWebSocket::disconnected, &m_socket,
                                 [this]()
                                 {
                                     m_opening = false;
                                     m_pingTimer.stop();
                                     ++m_disconnectGeneration;
                                     m_ignoredRequestIds.clear();
                                     qCInfo(logJmapWebSocketTransport) << "disconnected";
                                 });
                QObject::connect(&m_socket, &QWebSocket::errorOccurred, &m_socket,
                                 [this](QAbstractSocket::SocketError)
                                 {
                                     m_opening = false;
                                     qCWarning(logJmapWebSocketTransport).noquote()
                                         << "socket error" << m_socket.errorString();
                                 });
                QObject::connect(&m_socket, &QWebSocket::textMessageReceived, &m_socket,
                                 [this](const QString& message) { bufferMessage(message); });
            }

            ~WebSocketJmapConnection()
            {
                m_socket.abort();
            }

            void invalidate()
            {
                m_opening = false;
                m_pingTimer.stop();
                ++m_disconnectGeneration;
                m_responses.clear();
                m_ignoredRequestIds.clear();
                m_invalidMessageDescription.clear();
                m_socket.abort();
            }

            [[nodiscard]] QCoro::Task<WebSocketAttemptResult>
            call(const std::string_view webSocketUrl, const JmapMethodRequest& request,
                 const detail::JmapRequestLogContext& logContext)
            {
                if (request.cancellation.isCancellationRequested())
                {
                    co_return WebSocketAttemptResult{
                        .result =
                            cancelledError("JMAP method call cancelled before WebSocket dispatch"),
                    };
                }

                const std::string requestId = nextMethodRequestId();
                const auto payload = serializeWebSocketRequestEnvelope(request.envelope, requestId);
                if (!payload.has_value())
                {
                    co_return WebSocketAttemptResult{
                        .result = invalidRequestError("Failed to serialize JMAP request envelope"),
                    };
                }

                MethodRequestTrace trace{QString::fromStdString(requestId),
                                         QStringLiteral("websocket"), logContext};
                const auto finish = [&](JmapMethodTransportResult result,
                                        const bool requestDispatched,
                                        const bool validWebSocketResponse)
                {
                    return WebSocketAttemptResult{
                        .result = trace.finish(std::move(result)),
                        .requestDispatched = requestDispatched,
                        .validWebSocketResponse = validWebSocketResponse,
                    };
                };

                const auto connected = co_await ensureConnected(webSocketUrl, request.accessToken,
                                                                request.cancellation);
                if (const auto* error = std::get_if<TransportError>(&connected))
                {
                    co_return finish(*error, false, false);
                }

                QElapsedTimer responseElapsed;
                responseElapsed.start();
                const auto disconnectGeneration = m_disconnectGeneration;
                const auto invalidMessageGeneration = m_invalidMessageGeneration;
                const qint64 bytesQueued =
                    m_socket.sendTextMessage(QString::fromStdString(*payload));
                if (bytesQueued < 0)
                {
                    co_return finish(networkError("Failed to queue JMAP WebSocket request"), false,
                                     false);
                }
                if (request.dispatched)
                    request.dispatched();

                while (
                    responseElapsed.elapsed() <
                    std::chrono::duration_cast<std::chrono::milliseconds>(responseTimeout).count())
                {
                    const auto response = m_responses.find(requestId);
                    if (response != m_responses.end())
                    {
                        auto buffered = std::move(response->second);
                        m_responses.erase(response);
                        m_ignoredRequestIds.erase(requestId);

                        if (buffered.type == "RequestError")
                        {
                            WebSocketMessageHeader header;
                            auto json = buffered.payload;
                            if (glz::read<glz::opts{.error_on_unknown_keys = false}>(header, json))
                            {
                                co_return finish(
                                    decodingError("Failed to parse JMAP WebSocket request error"),
                                    true, false);
                            }
                            co_return finish(
                                invalidResponseError(requestErrorMessage(header).toStdString()),
                                true, true);
                        }

                        const auto parsed = parseResponseEnvelope(buffered.payload);
                        if (!parsed.ok())
                        {
                            co_return finish(
                                decodingError(parsed.error.value_or(
                                    "Failed to parse JMAP WebSocket response envelope")),
                                true, false);
                        }
                        co_return finish(std::move(*parsed.value), true, true);
                    }

                    if (request.cancellation.isCancellationRequested())
                    {
                        m_ignoredRequestIds.insert(requestId);
                        co_return finish(
                            cancelledError(
                                "JMAP method call cancelled while awaiting WebSocket response"),
                            true, false);
                    }
                    if (m_socket.state() != QAbstractSocket::ConnectedState ||
                        m_disconnectGeneration != disconnectGeneration)
                    {
                        m_ignoredRequestIds.insert(requestId);
                        co_return finish(
                            networkError("JMAP WebSocket disconnected before the response arrived"),
                            true, false);
                    }
                    if (m_invalidMessageGeneration != invalidMessageGeneration)
                    {
                        m_ignoredRequestIds.insert(requestId);
                        auto message =
                            std::string{"JMAP WebSocket returned an invalid response message"};
                        if (!m_invalidMessageDescription.isEmpty())
                        {
                            message += ": ";
                            message += m_invalidMessageDescription.toStdString();
                        }
                        co_return finish(decodingError(message), true, false);
                    }

                    static_cast<void>(co_await qCoro(&m_socket, &QWebSocket::textMessageReceived,
                                                     cancellationPollInterval));
                }

                m_ignoredRequestIds.insert(requestId);
                co_return finish(networkError("Timed out waiting for JMAP WebSocket response"),
                                 true, false);
            }

          private:
            [[nodiscard]] QCoro::Task<std::variant<std::monostate, TransportError>>
            ensureConnected(const std::string_view webSocketUrl, const std::string_view accessToken,
                            const CancellationToken& cancellation)
            {
                const QUrl url{QString::fromStdString(std::string{webSocketUrl})};
                if (!url.isValid() ||
                    url.scheme().compare(QStringLiteral("wss"), Qt::CaseInsensitive) != 0)
                {
                    co_return decodingError(
                        "The advertised JMAP WebSocket endpoint is not a valid wss URL");
                }

                const std::string nextUrl{webSocketUrl};
                const std::string nextToken{accessToken};
                if (m_url != nextUrl || m_accessToken != nextToken)
                {
                    m_socket.abort();
                    m_responses.clear();
                    m_ignoredRequestIds.clear();
                    m_invalidMessageDescription.clear();
                    m_url = nextUrl;
                    m_accessToken = nextToken;
                    m_opening = false;
                }

                if (m_socket.state() == QAbstractSocket::ConnectedState &&
                    m_socket.subprotocol() == QStringLiteral("jmap"))
                {
                    co_return std::monostate{};
                }

                if (!m_opening)
                {
                    QNetworkRequest handshake{url};
                    handshake.setRawHeader("Authorization",
                                           bearerAuthorizationValue(m_accessToken));
                    QWebSocketHandshakeOptions options;
                    options.setSubprotocols({QStringLiteral("jmap")});
                    m_opening = true;
                    m_socket.open(handshake, options);
                    qCInfo(logJmapWebSocketTransport) << "connecting";
                }

                QElapsedTimer elapsed;
                elapsed.start();
                const auto timeoutMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(connectTimeout).count();
                while (elapsed.elapsed() < timeoutMs)
                {
                    if (cancellation.isCancellationRequested())
                    {
                        co_return cancelledError(
                            "JMAP method call cancelled while connecting WebSocket");
                    }
                    if (m_socket.state() == QAbstractSocket::ConnectedState)
                    {
                        m_opening = false;
                        if (m_socket.subprotocol() != QStringLiteral("jmap"))
                        {
                            m_socket.abort();
                            co_return decodingError(
                                "Server did not negotiate the jmap WebSocket subprotocol");
                        }
                        co_return std::monostate{};
                    }
                    if (!m_opening && m_socket.state() == QAbstractSocket::UnconnectedState)
                    {
                        const auto error = m_socket.errorString();
                        co_return networkError(
                            error.isEmpty() ? std::string_view{"JMAP WebSocket handshake failed"}
                                            : std::string_view{error.toStdString()});
                    }

                    const auto remaining = std::chrono::milliseconds{
                        std::max<qint64>(1, timeoutMs - elapsed.elapsed())};
                    static_cast<void>(
                        co_await qCoro(&m_socket, &QWebSocket::stateChanged,
                                       std::min(remaining, cancellationPollInterval)));
                }

                m_opening = false;
                m_socket.abort();
                co_return networkError("Timed out establishing JMAP WebSocket");
            }

            void bufferMessage(const QString& message)
            {
                WebSocketMessageHeader header;
                auto json = message.toStdString();
                if (glz::read<glz::opts{.error_on_unknown_keys = false}>(header, json) ||
                    !header.type.has_value())
                {
                    const auto frameDescription = detail::describeWebSocketFrame(json);
                    ++m_invalidMessageGeneration;
                    m_invalidMessageDescription = frameDescription;
                    qCWarning(logJmapWebSocketTransport).noquote()
                        << "invalid WebSocket message" << frameDescription;
                    return;
                }

                if (*header.type != "Response" && *header.type != "RequestError")
                {
                    qCDebug(logJmapWebSocketTransport).noquote()
                        << "ignoring unsolicited message type"
                        << QString::fromStdString(*header.type);
                    return;
                }
                if (!header.requestId.has_value() || header.requestId->empty())
                {
                    const auto frameDescription = detail::describeWebSocketFrame(json);
                    ++m_invalidMessageGeneration;
                    m_invalidMessageDescription = frameDescription;
                    qCWarning(logJmapWebSocketTransport).noquote()
                        << "response omitted required requestId" << frameDescription;
                    return;
                }
                if (m_ignoredRequestIds.erase(*header.requestId) > 0)
                {
                    return;
                }

                m_responses.insert_or_assign(*header.requestId, BufferedWebSocketMessage{
                                                                    .type = *header.type,
                                                                    .payload = std::move(json),
                                                                });
            }

            QWebSocket m_socket;
            QTimer m_pingTimer;
            std::string m_url;
            std::string m_accessToken;
            std::unordered_map<std::string, BufferedWebSocketMessage> m_responses;
            std::unordered_set<std::string> m_ignoredRequestIds;
            QString m_invalidMessageDescription;
            std::uint64_t m_disconnectGeneration = 0;
            std::uint64_t m_invalidMessageGeneration = 0;
            bool m_opening = false;
        };
    } // namespace

    WebSocketFailureCooldowns::WebSocketFailureCooldowns(
        const std::chrono::milliseconds failureCooldown)
        : m_failureCooldown(failureCooldown)
    {
    }

    std::optional<std::chrono::milliseconds>
    WebSocketFailureCooldowns::retryDelay(const std::string_view url) const
    {
        const auto found = m_retryAfter.find(std::string{url});
        if (found == m_retryAfter.end())
            return std::nullopt;
        const auto remaining = found->second - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero())
            return std::nullopt;
        return std::chrono::ceil<std::chrono::milliseconds>(remaining);
    }

    void WebSocketFailureCooldowns::recordFailure(std::string url)
    {
        m_retryAfter.insert_or_assign(std::move(url),
                                      std::chrono::steady_clock::now() + m_failureCooldown);
    }

    void WebSocketFailureCooldowns::recordSuccess(const std::string_view url)
    {
        m_retryAfter.erase(std::string{url});
    }

    void JmapMethodTransport::invalidateConnection(const std::string_view accountId)
    {
        static_cast<void>(accountId);
    }

    RefreshingJmapMethodTransport::RefreshingJmapMethodTransport(JmapMethodTransport& transport)
        : m_transport(transport)
    {
    }

    void RefreshingJmapMethodTransport::setAccessTokenProvider(
        javelin::jmap::auth::AccessTokenProvider provider)
    {
        m_accessTokenProvider = std::move(provider);
    }

    void RefreshingJmapMethodTransport::setRefreshHandler(
        javelin::jmap::auth::AccessTokenRefreshHandler handler)
    {
        m_refreshHandler = std::move(handler);
    }

    void RefreshingJmapMethodTransport::invalidateConnection(const std::string_view accountId)
    {
        m_transport.invalidateConnection(accountId);
    }

    QCoro::Task<JmapMethodTransportResult>
    RefreshingJmapMethodTransport::call(JmapMethodRequest request)
    {
        if (m_accessTokenProvider)
        {
            const auto currentAccessToken = m_accessTokenProvider(request.accountId);
            if (currentAccessToken.has_value() && *currentAccessToken != request.accessToken)
                request.accessToken = *currentAccessToken;
        }

        auto retryRequest = request;
        auto result = co_await m_transport.call(std::move(request));
        const auto* error = std::get_if<TransportError>(&result);
        if (error == nullptr || error->code != TransportErrorCode::HttpFailure ||
            error->httpStatus != 401 || !m_refreshHandler)
        {
            co_return result;
        }

        const auto rejectedAccessToken = retryRequest.accessToken;
        auto refreshedAccessToken =
            co_await m_refreshHandler(retryRequest.accountId, rejectedAccessToken);
        if (!refreshedAccessToken.has_value() || *refreshedAccessToken == rejectedAccessToken)
        {
            co_return result;
        }
        if (retryRequest.cancellation.isCancellationRequested())
        {
            co_return cancelledError("JMAP method call cancelled while refreshing authentication");
        }

        retryRequest.accessToken = std::move(*refreshedAccessToken);
        retryRequest.dispatched = {};
        co_return co_await m_transport.call(std::move(retryRequest));
    }

    HttpJmapMethodTransport::HttpJmapMethodTransport(AbstractTransport& transport)
        : m_transport(transport)
    {
    }

    void HttpJmapMethodTransport::invalidateConnection(const std::string_view accountId)
    {
        static_cast<void>(accountId);
        m_transport.invalidateConnections();
    }

    QCoro::Task<JmapMethodTransportResult> HttpJmapMethodTransport::call(JmapMethodRequest request)
    {
        if (request.cancellation.isCancellationRequested())
        {
            co_return cancelledError("JMAP method call cancelled before HTTP dispatch");
        }

        const auto body = serializeRequestEnvelope(request.envelope);
        if (!body.has_value())
        {
            co_return invalidRequestError("Failed to serialize JMAP request envelope");
        }

        MethodRequestTrace trace{QString::fromStdString(nextMethodRequestId()),
                                 QStringLiteral("http"), detail::describeJmapRequest(request)};
        const auto result = co_await m_transport.send(HttpRequest{
            .method = HttpMethod::Post,
            .url = QUrl{QString::fromStdString(request.apiUrl)},
            .headers =
                {
                    HttpHeader{.name = "Authorization",
                               .value = bearerAuthorizationValue(request.accessToken)},
                    HttpHeader{.name = "Accept", .value = "application/json"},
                    HttpHeader{.name = "Content-Type", .value = "application/json"},
                },
            .body = QByteArray::fromStdString(*body),
            .authentication = {},
            .cancellation = std::move(request.cancellation),
            .dispatched = std::move(request.dispatched),
        });
        if (const auto* error = std::get_if<TransportError>(&result))
        {
            co_return trace.finish(*error);
        }

        const auto parsed =
            parseResponseEnvelope(std::get<HttpResponse>(result).body.toStdString());
        if (!parsed.ok())
        {
            co_return trace.finish(invalidResponseError(*parsed.error));
        }
        co_return trace.finish(std::move(*parsed.value));
    }

    struct PreferredJmapMethodTransport::Impl
    {
        javelin::jmap::cache::DatabaseConnection& databaseConnection;
        HttpJmapMethodTransport& httpTransport;
        WebSocketFailureCooldowns& cooldowns;
        std::unordered_map<std::string, std::unique_ptr<WebSocketJmapConnection>> connections;
    };

    PreferredJmapMethodTransport::PreferredJmapMethodTransport(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        HttpJmapMethodTransport& httpTransport, WebSocketFailureCooldowns& cooldowns)
        : m_impl(std::make_unique<Impl>(Impl{
              .databaseConnection = databaseConnection,
              .httpTransport = httpTransport,
              .cooldowns = cooldowns,
              .connections = {},
          }))
    {
    }

    PreferredJmapMethodTransport::~PreferredJmapMethodTransport() = default;

    void PreferredJmapMethodTransport::invalidateConnection(const std::string_view accountId)
    {
        m_impl->httpTransport.invalidateConnection(accountId);

        javelin::jmap::cache::JmapTransportPreferenceRepository preferences{
            m_impl->databaseConnection};
        const auto targetResult = preferences.resolve(accountId);
        const auto* target =
            std::get_if<std::optional<javelin::jmap::cache::JmapTransportTarget>>(&targetResult);
        if (target == nullptr || !target->has_value())
            return;

        const auto connection = m_impl->connections.find((*target)->ownerAccountId);
        if (connection == m_impl->connections.end() || connection->second == nullptr)
            return;

        connection->second->invalidate();
        qCInfo(logJmapWebSocketTransport) << "invalidated connection after network discontinuity";
    }

    QCoro::Task<JmapMethodTransportResult>
    PreferredJmapMethodTransport::call(JmapMethodRequest request)
    {
        if (request.accountId.empty())
        {
            co_return co_await m_impl->httpTransport.call(std::move(request));
        }

        javelin::jmap::cache::JmapTransportPreferenceRepository preferences{
            m_impl->databaseConnection};
        const auto targetResult = preferences.resolve(request.accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&targetResult))
        {
            qCWarning(logJmapWebSocketTransport).noquote()
                << "could not resolve transport preference; using HTTP" << error->message;
            if (request.transportPolicy == JmapTransportPolicy::ForceWebSocket)
            {
                co_return networkError(
                    "WebSocket is required, but its endpoint could not be resolved from the cache");
            }
            co_return co_await m_impl->httpTransport.call(std::move(request));
        }

        const auto& target =
            std::get<std::optional<javelin::jmap::cache::JmapTransportTarget>>(targetResult);
        const bool forceWebSocket = request.transportPolicy == JmapTransportPolicy::ForceWebSocket;
        if (!target.has_value())
        {
            if (forceWebSocket)
            {
                qCWarning(logJmapWebSocketTransport)
                    << "forced WebSocket request has no advertised endpoint";
                co_return networkError("WebSocket is required, but this account has no advertised "
                                       "JMAP WebSocket endpoint");
            }
            co_return co_await m_impl->httpTransport.call(std::move(request));
        }
        if (!forceWebSocket && m_impl->cooldowns.retryDelay(target->webSocketUrl).has_value())
        {
            co_return co_await m_impl->httpTransport.call(std::move(request));
        }

        auto& connection = m_impl->connections[target->ownerAccountId];
        if (connection == nullptr)
        {
            connection = std::make_unique<WebSocketJmapConnection>();
        }

        const auto logContext = detail::describeJmapRequest(m_impl->databaseConnection, request);
        auto attempt = co_await connection->call(target->webSocketUrl, request, logContext);
        if (attempt.validWebSocketResponse)
        {
            m_impl->cooldowns.recordSuccess(target->webSocketUrl);
            co_return std::move(attempt.result);
        }

        const auto* transportError = std::get_if<TransportError>(&attempt.result);
        if (transportError == nullptr || transportError->code == TransportErrorCode::Cancelled)
        {
            co_return std::move(attempt.result);
        }

        m_impl->cooldowns.recordFailure(target->webSocketUrl);
        qCWarning(logJmapWebSocketTransport).noquote()
            << "WebSocket unavailable; preferring HTTP for future requests for 15 minutes"
            << QString::fromStdString(transportError->message);

        if (!attempt.requestDispatched)
        {
            if (forceWebSocket)
            {
                qCWarning(logJmapWebSocketTransport)
                    << "forced WebSocket request failed; HTTP fallback disabled";
                co_return std::move(attempt.result);
            }
            co_return co_await m_impl->httpTransport.call(std::move(request));
        }
        co_return std::move(attempt.result);
    }

} // namespace javelin::jmap::api
