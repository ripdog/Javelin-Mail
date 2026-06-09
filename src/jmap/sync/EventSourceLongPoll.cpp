#include "jmap/sync/EventSourceLongPoll.h"

#include "jmap/api/Error.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif
#include <QCoroNetworkReply>
#include <QCoroTimer>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <glaze/glaze.hpp>

#include <QByteArray>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopeGuard>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace javelin::jmap::sync
{

    struct RawStateChange
    {
        std::optional<std::string> type;
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> changed;
    };

} // namespace javelin::jmap::sync

template <> struct glz::meta<javelin::jmap::sync::RawStateChange>
{
    using T = javelin::jmap::sync::RawStateChange;

    static constexpr auto value = glz::object("@type", &T::type, "changed", &T::changed);
};

namespace javelin::jmap::sync
{

    namespace
    {
        enum class ParsedEventStatus
        {
            NeedMoreData,
            Ignored,
            Parsed,
            Invalid,
        };

        constexpr auto eventSourceIdleTimeout = std::chrono::seconds{620};

        [[nodiscard]] QByteArray summarizeBody(const QByteArray& body)
        {
            constexpr qsizetype maxBytes = 4096;
            if (body.size() <= maxBytes)
            {
                return body;
            }

            return body.first(maxBytes) + "...";
        }

        [[nodiscard]] const char* parsedEventStatusName(const ParsedEventStatus status)
        {
            switch (status)
            {
            case ParsedEventStatus::NeedMoreData:
                return "need-more-data";
            case ParsedEventStatus::Ignored:
                return "ignored";
            case ParsedEventStatus::Parsed:
                return "parsed";
            case ParsedEventStatus::Invalid:
                return "invalid";
            }
            return "unknown";
        }

        struct ParsedEvent
        {
            ParsedEventStatus status = ParsedEventStatus::NeedMoreData;
            std::optional<LongPollResponse> response;
            std::optional<std::string> errorMessage;
        };

        [[nodiscard]] javelin::jmap::api::TransportError makeTransportError(
            const javelin::jmap::api::TransportErrorCode code, std::string message,
            const std::optional<int> httpStatus = std::nullopt)
        {
            return javelin::jmap::api::TransportError{
                .code = code,
                .message = std::move(message),
                .httpStatus = httpStatus,
            };
        }

        [[nodiscard]] javelin::jmap::api::TransportError mapReplyError(QNetworkReply& reply)
        {
            if (reply.error() == QNetworkReply::OperationCanceledError)
            {
                return makeTransportError(javelin::jmap::api::TransportErrorCode::Cancelled,
                                          reply.errorString().toStdString());
            }

            const auto statusCode = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute);
            return makeTransportError(
                javelin::jmap::api::TransportErrorCode::NetworkFailure,
                reply.errorString().toStdString(),
                statusCode.isValid() ? std::optional{statusCode.toInt()} : std::nullopt);
        }

        [[nodiscard]] QString encodeTemplateValue(const std::string_view value)
        {
            return QString::fromUtf8(
                QUrl::toPercentEncoding(QString::fromStdString(std::string{value})));
        }

        [[nodiscard]] std::optional<QUrl> buildEventSourceUrl(const LongPollRequest& request)
        {
            QString expanded = QString::fromStdString(request.eventSourceUrl);

            std::string types = "*";
            if (!request.types.empty())
            {
                types.clear();
                for (std::size_t index = 0; index < request.types.size(); ++index)
                {
                    if (index > 0)
                    {
                        types += ",";
                    }
                    types += request.types[index];
                }
            }

            const bool hadTemplateVariables =
                expanded.contains(QStringLiteral("{types}")) ||
                expanded.contains(QStringLiteral("{closeafter}")) ||
                expanded.contains(QStringLiteral("{ping}")) ||
                expanded.contains(QStringLiteral("{?types,closeafter,ping}"));

            expanded.replace(QStringLiteral("{?types,closeafter,ping}"),
                             QStringLiteral("?types=%1&closeafter=no&ping=300")
                                 .arg(encodeTemplateValue(types)));
            expanded.replace(QStringLiteral("{types}"), encodeTemplateValue(types));
            expanded.replace(QStringLiteral("{closeafter}"), QStringLiteral("no"));
            expanded.replace(QStringLiteral("{ping}"), QStringLiteral("300"));

            QUrl url{expanded};
            if (!url.isValid() || url.isEmpty())
            {
                return std::nullopt;
            }

            if (!hadTemplateVariables)
            {
                QUrlQuery query{url};
                query.addQueryItem(QStringLiteral("types"), QString::fromStdString(types));
                query.addQueryItem(QStringLiteral("closeafter"), QStringLiteral("no"));
                query.addQueryItem(QStringLiteral("ping"), QStringLiteral("300"));
                url.setQuery(query);
            }

            return url;
        }

        [[nodiscard]] ParsedEvent parseStateEvent(const std::string_view accountId,
                                                  const std::string_view fallbackState,
                                                  const std::string_view eventName,
                                                  const std::string_view eventId,
                                                  const std::string_view eventData)
        {
            if (eventName.empty() || eventName == "message")
            {
                return ParsedEvent{
                    .status = ParsedEventStatus::Ignored,
                    .response = std::nullopt,
                    .errorMessage = std::nullopt,
                };
            }

            if (eventName == "ping")
            {
                return ParsedEvent{
                    .status = ParsedEventStatus::Ignored,
                    .response = std::nullopt,
                    .errorMessage = std::nullopt,
                };
            }

            if (eventName != "state")
            {
                return ParsedEvent{
                    .status = ParsedEventStatus::Ignored,
                    .response = std::nullopt,
                    .errorMessage = std::nullopt,
                };
            }

            RawStateChange stateChange;
            std::string buffer = std::string{eventData};
            if (const auto readError =
                    glz::read<glz::opts{.error_on_unknown_keys = false}>(stateChange, buffer))
            {
                return ParsedEvent{
                    .status = ParsedEventStatus::Invalid,
                    .response = std::nullopt,
                    .errorMessage = std::string{glz::format_error(readError, buffer)},
                };
            }

            if (stateChange.type.has_value() && *stateChange.type != "StateChange")
            {
                return ParsedEvent{
                    .status = ParsedEventStatus::Invalid,
                    .response = std::nullopt,
                    .errorMessage = std::string{"Expected a StateChange event payload."},
                };
            }

            const auto accountIt = stateChange.changed.find(std::string{accountId});
            if (accountIt == stateChange.changed.end())
            {
                return ParsedEvent{
                    .status = ParsedEventStatus::Ignored,
                    .response = std::nullopt,
                    .errorMessage = std::nullopt,
                };
            }

            std::vector<std::string> changedTypes;
            changedTypes.reserve(accountIt->second.size());
            for (const auto& [typeName, state] : accountIt->second)
            {
                static_cast<void>(state);
                changedTypes.push_back(typeName);
            }

            return ParsedEvent{
                .status = ParsedEventStatus::Parsed,
                .response =
                    LongPollResponse{
                        .newState = eventId.empty() ? std::string{fallbackState}
                                                    : std::string{eventId},
                        .changedTypes = std::move(changedTypes),
                    },
                .errorMessage = std::nullopt,
            };
        }

    } // namespace

} // namespace javelin::jmap::sync

namespace javelin::jmap::sync
{

    QCoro::Task<void> QtLongPollSleeper::sleepFor(const std::chrono::milliseconds delay)
    {
        if (delay.count() <= 0)
        {
            co_return;
        }

        QTimer timer;
        timer.setSingleShot(true);
        timer.start(delay);
        co_await qCoro(timer).waitForTimeout();
    }

    EventSourceLongPollChannel::EventSourceLongPollChannel(
        QNetworkAccessManager& networkAccessManager, std::string accessToken,
        LongPollStatusCallback statusCallback)
        : m_networkAccessManager(networkAccessManager), m_accessToken(std::move(accessToken)),
          m_statusCallback(std::move(statusCallback))
    {
    }

    EventSourceLongPollChannel::~EventSourceLongPollChannel()
    {
        cancel();
    }

    void EventSourceLongPollChannel::cancel()
    {
        if (m_activeReply.isNull())
        {
            return;
        }

        qInfo().noquote() << "Long poll aborting active event-source request"
                          << m_activeReply->url().toString();
        m_activeReply->abort();
    }

    QCoro::Task<LongPollResult> EventSourceLongPollChannel::poll(const LongPollRequest& request)
    {
        const auto url = buildEventSourceUrl(request);
        if (!url.has_value())
        {
            qWarning().noquote() << "Long poll failed to build event-source URL"
                                 << QString::fromStdString(request.eventSourceUrl);
            co_return makeTransportError(
                javelin::jmap::api::TransportErrorCode::ResponseDecodingFailed,
                "Failed to expand the eventSourceUrl URI template.");
        }

        QNetworkRequest networkRequest{*url};
        networkRequest.setTransferTimeout(0);
        networkRequest.setRawHeader("Accept", "text/event-stream");
        networkRequest.setRawHeader("Authorization",
                                    QByteArray{"Bearer "} +
                                        QByteArray::fromStdString(m_accessToken));
        if (!request.lastState.empty())
        {
            networkRequest.setRawHeader("Last-Event-ID",
                                        QByteArray::fromStdString(request.lastState));
        }

        QNetworkReply* reply = m_networkAccessManager.get(networkRequest);
        m_activeReply = reply;
        const auto deleteReply = qScopeGuard(
            [this, reply]()
            {
                if (m_activeReply == reply)
                {
                    m_activeReply.clear();
                }
                if (reply != nullptr)
                {
                    reply->deleteLater();
                }
            });

        bool connectedReported = false;
        QObject::connect(reply, &QObject::destroyed, reply,
                         [this, reply]()
                         {
                             if (m_activeReply == reply)
                             {
                                 m_activeReply.clear();
                             }
                         });
        QObject::connect(reply, &QNetworkReply::metaDataChanged, reply,
                         [this, reply, &connectedReported]()
                         {
                             if (connectedReported)
                             {
                                 return;
                             }

                             const auto statusAttribute =
                                 reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
                             const int statusCode =
                                 statusAttribute.isValid() ? statusAttribute.toInt() : 0;
                             if (statusCode >= 200 && statusCode < 400 && m_statusCallback)
                             {
                                 connectedReported = true;
                                 m_statusCallback(LongPollConnectionStatus::Connected);
                             }
                         });

        QByteArray pendingBuffer;
        std::string eventName;
        std::string eventId;
        std::string eventData;

        const auto resetEvent =
            [&eventName, &eventId, &eventData]()
            {
                eventName.clear();
                eventId.clear();
                eventData.clear();
            };

        const auto finalizeEvent = [&]() -> std::optional<LongPollResult>
        {
            if (eventName.empty() && eventId.empty() && eventData.empty())
            {
                return std::nullopt;
            }

            const auto parsed =
                parseStateEvent(request.accountId, request.lastState, eventName, eventId, eventData);
            qInfo().noquote() << "Long poll received SSE event"
                              << "status" << parsedEventStatusName(parsed.status) << "event"
                              << QString::fromStdString(eventName) << "id"
                              << QString::fromStdString(eventId) << "data"
                              << summarizeBody(QByteArray::fromStdString(eventData));
            if (parsed.status == ParsedEventStatus::Invalid)
            {
                qWarning().noquote() << "Long poll invalid event payload"
                                     << QString::fromStdString(eventName)
                                     << QString::fromStdString(eventId)
                                     << summarizeBody(QByteArray::fromStdString(eventData));
            }
            resetEvent();

            switch (parsed.status)
            {
            case ParsedEventStatus::NeedMoreData:
            case ParsedEventStatus::Ignored:
                return std::nullopt;
            case ParsedEventStatus::Parsed:
                return *parsed.response;
            case ParsedEventStatus::Invalid:
                return makeTransportError(
                    javelin::jmap::api::TransportErrorCode::ResponseDecodingFailed,
                    parsed.errorMessage.value_or("Failed to parse event-source state event."));
            }

            return std::nullopt;
        };

        while (true)
        {
            if (reply->error() != QNetworkReply::NoError && reply->isFinished())
            {
                qWarning().noquote() << "Long poll network error" << reply->url().toString()
                                     << reply->error() << reply->errorString()
                                     << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                                            .toInt()
                                     << summarizeBody(reply->readAll());
                co_return mapReplyError(*reply);
            }

            const auto statusAttribute = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
            const int statusCode = statusAttribute.isValid() ? statusAttribute.toInt() : 0;
            if (reply->isFinished() && statusCode >= 400)
            {
                const QByteArray responseBody = reply->readAll();
                qWarning().noquote() << "Long poll HTTP failure" << reply->url().toString()
                                     << statusCode << summarizeBody(responseBody);
                co_return makeTransportError(javelin::jmap::api::TransportErrorCode::HttpFailure,
                                             reply->errorString().toStdString(), statusCode);
            }

            if (!reply->isFinished() && reply->bytesAvailable() == 0)
            {
                const bool ready = co_await qCoro(reply).waitForReadyRead(eventSourceIdleTimeout);
                if (!ready && !reply->isFinished())
                {
                    reply->abort();
                    qWarning().noquote() << "Long poll timed out waiting for event-source activity"
                                         << reply->url().toString();
                    co_return makeTransportError(
                        javelin::jmap::api::TransportErrorCode::NetworkFailure,
                        "Timed out waiting for event-source activity.");
                }

                if (!ready && reply->isFinished())
                {
                    const QByteArray chunk = reply->readAll();
                    qInfo().noquote() << "Long poll received raw event-source bytes"
                                      << reply->url().toString() << chunk.size()
                                      << summarizeBody(chunk);
                    pendingBuffer += chunk;
                }
                else if (ready)
                {
                    const QByteArray chunk = reply->readAll();
                    qInfo().noquote() << "Long poll received raw event-source bytes"
                                      << reply->url().toString() << chunk.size()
                                      << summarizeBody(chunk);
                    pendingBuffer += chunk;
                }
            }
            else
            {
                const QByteArray chunk = reply->readAll();
                qInfo().noquote() << "Long poll received raw event-source bytes"
                                  << reply->url().toString() << chunk.size()
                                  << summarizeBody(chunk);
                pendingBuffer += chunk;
            }

            while (true)
            {
                const qsizetype newlineIndex = pendingBuffer.indexOf('\n');
                if (newlineIndex < 0)
                {
                    break;
                }

                QByteArray line = pendingBuffer.left(newlineIndex);
                pendingBuffer.remove(0, newlineIndex + 1);
                if (line.endsWith('\r'))
                {
                    line.chop(1);
                }

                if (line.isEmpty())
                {
                    if (const auto parsed = finalizeEvent(); parsed.has_value())
                    {
                        co_return *parsed;
                    }
                    continue;
                }

                if (line.startsWith(':'))
                {
                    continue;
                }

                const qsizetype separatorIndex = line.indexOf(':');
                const QByteArray field = separatorIndex >= 0 ? line.left(separatorIndex) : line;
                QByteArray value =
                    separatorIndex >= 0 ? line.mid(separatorIndex + 1) : QByteArray{};
                if (value.startsWith(' '))
                {
                    value.remove(0, 1);
                }

                if (field == "event")
                {
                    eventName = value.toStdString();
                }
                else if (field == "data")
                {
                    if (!eventData.empty())
                    {
                        eventData += '\n';
                    }
                    eventData += value.toStdString();
                }
                else if (field == "id")
                {
                    eventId = value.toStdString();
                }
            }

            if (reply->isFinished())
            {
                if (!pendingBuffer.isEmpty())
                {
                    pendingBuffer += '\n';
                    continue;
                }

                if (const auto parsed = finalizeEvent(); parsed.has_value())
                {
                    co_return *parsed;
                }

                qWarning().noquote()
                    << "Long poll closed before a state event was received" << reply->url().toString()
                    << statusCode << summarizeBody(pendingBuffer);
                co_return makeTransportError(
                    javelin::jmap::api::TransportErrorCode::NetworkFailure,
                    "Event-source connection closed before a state event was received.",
                    statusCode > 0 ? std::optional{statusCode} : std::nullopt);
            }
        }
    }

} // namespace javelin::jmap::sync
