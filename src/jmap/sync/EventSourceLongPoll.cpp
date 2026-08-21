#include "jmap/sync/EventSourceLongPoll.h"

#include "jmap/api/Error.h"
#include "jmap/sync/PushActivityTracker.h"
#include "jmap/sync/PushProtocol.h"
#include "jmap/sync/PushStreamSession.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif
#include <QCoroNetworkReply>
#include <QCoroTimer>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <QByteArray>
#include <QDebug>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopeGuard>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>

namespace javelin::jmap::sync
{
    Q_LOGGING_CATEGORY(logEventSource, "jmap.push.http")

    namespace
    {
        [[nodiscard]] QByteArray summarizeBody(const QByteArray& body)
        {
            constexpr qsizetype maxBytes = 4096;
            if (body.size() <= maxBytes)
            {
                return body;
            }

            return body.first(maxBytes) + "...";
        }

        [[nodiscard]] javelin::jmap::api::TransportError
        makeTransportError(const javelin::jmap::api::TransportErrorCode code, std::string message,
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
            return makeTransportError(javelin::jmap::api::TransportErrorCode::NetworkFailure,
                                      reply.errorString().toStdString(),
                                      statusCode.isValid() ? std::optional{statusCode.toInt()}
                                                           : std::nullopt);
        }

        [[nodiscard]] QString encodeTemplateValue(const std::string_view value)
        {
            return QString::fromUtf8(
                QUrl::toPercentEncoding(QString::fromStdString(std::string{value})));
        }

        [[nodiscard]] std::optional<QUrl>
        buildEventSourceUrl(const std::string_view eventSourceUrl,
                            const StateChangeSubscription& subscription)
        {
            QString expanded = QString::fromStdString(std::string{eventSourceUrl});

            std::string types = "*";
            if (!subscription.types.empty())
            {
                types.clear();
                for (std::size_t index = 0; index < subscription.types.size(); ++index)
                {
                    if (index > 0)
                    {
                        types += ",";
                    }
                    types += subscription.types[index];
                }
            }

            const bool hadTemplateVariables =
                expanded.contains(QStringLiteral("{types}")) ||
                expanded.contains(QStringLiteral("{closeafter}")) ||
                expanded.contains(QStringLiteral("{ping}")) ||
                expanded.contains(QStringLiteral("{?types,closeafter,ping}"));

            expanded.replace(QStringLiteral("{?types,closeafter,ping}"),
                             QStringLiteral("?types=%1&closeafter=no&ping=%2")
                                 .arg(encodeTemplateValue(types))
                                 .arg(requestedPushPingInterval.count()));
            expanded.replace(QStringLiteral("{types}"), encodeTemplateValue(types));
            expanded.replace(QStringLiteral("{closeafter}"), QStringLiteral("no"));
            expanded.replace(QStringLiteral("{ping}"),
                             QString::number(requestedPushPingInterval.count()));

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
                query.addQueryItem(QStringLiteral("ping"),
                                   QString::number(requestedPushPingInterval.count()));
                url.setQuery(query);
            }

            return url;
        }

    } // namespace

} // namespace javelin::jmap::sync

namespace javelin::jmap::sync
{

    QCoro::Task<void> QtStateChangeSleeper::sleepFor(const std::chrono::milliseconds delay)
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

    EventSourceStateChangeSource::EventSourceStateChangeSource(
        QNetworkAccessManager& networkAccessManager, std::string eventSourceUrl,
        std::string accessToken, StateChangeStatusCallback statusCallback)
        : m_networkAccessManager(networkAccessManager), m_eventSourceUrl(std::move(eventSourceUrl)),
          m_accessToken(std::move(accessToken)), m_statusCallback(std::move(statusCallback))
    {
    }

    EventSourceStateChangeSource::~EventSourceStateChangeSource()
    {
        cancel();
    }

    void EventSourceStateChangeSource::cancel()
    {
        auto* activeReply = m_activeReply.data();
        if (activeReply == nullptr)
        {
            return;
        }

        qInfo() << "State-change source aborting active event-source request";
        activeReply->abort();
    }

    QCoro::Task<StateChangeSourceResult>
    EventSourceStateChangeSource::consume(StateChangeSubscription subscription,
                                          StateChangeConsumer& consumer,
                                          StateChangeCancellation& cancellation)
    {
        const auto url = buildEventSourceUrl(m_eventSourceUrl, subscription);
        if (!url.has_value())
        {
            qWarning().noquote() << "State-change source failed to build event-source URL"
                                 << QString::fromStdString(m_eventSourceUrl);
            co_return makeTransportError(
                javelin::jmap::api::TransportErrorCode::ResponseDecodingFailed,
                "Failed to expand the eventSourceUrl URI template.");
        }

        QNetworkRequest networkRequest{*url};
        networkRequest.setTransferTimeout(0);
        networkRequest.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        networkRequest.setRawHeader("Accept", "text/event-stream");
        networkRequest.setRawHeader("Authorization", QByteArray{"Bearer "} +
                                                         QByteArray::fromStdString(m_accessToken));
        if (!subscription.lastState.empty())
        {
            networkRequest.setRawHeader("Last-Event-ID",
                                        QByteArray::fromStdString(subscription.lastState));
        }

        QNetworkReply* reply = m_networkAccessManager.get(networkRequest);
        m_activeReply = reply;
        PushActivityTracker activity{m_statusCallback, *url, maximumPushActivityTimeout};
        const auto deleteReply = qScopeGuard(
            [this, reply]()
            {
                if (m_activeReply == reply)
                {
                    m_activeReply.clear();
                }
                if (reply != nullptr)
                {
                    QObject::disconnect(reply, nullptr, reply, nullptr);
                    reply->deleteLater();
                }
            });

        QStringList subscribedTypes;
        for (const auto& type : subscription.types)
            subscribedTypes.push_back(QString::fromStdString(type));
        qCDebug(logEventSource).noquote()
            << "push subscription sent for" << subscribedTypes.join(QStringLiteral(", "))
            << activity.serverBaseUrl();

        QObject::connect(reply, &QObject::destroyed, reply,
                         [this, reply]()
                         {
                             if (m_activeReply == reply)
                             {
                                 m_activeReply.clear();
                             }
                         });
        bool connectedReported = false;
        QObject::connect(reply, &QNetworkReply::requestSent, reply,
                         [&activity, &connectedReported]()
                         {
                             if (!connectedReported)
                             {
                                 connectedReported = true;
                                 activity.recordActivity();
                             }
                         });

        QByteArray pendingBuffer;
        std::string eventName;
        std::string eventId;
        std::string eventData;

        const auto resetEvent = [&eventName, &eventId, &eventData]()
        {
            eventName.clear();
            eventId.clear();
            eventData.clear();
        };

        PushStreamSession stream{std::move(subscription), consumer};
        bool responseHeadersValidated = false;

        const auto finalizeEvent =
            [&]() -> QCoro::Task<std::optional<javelin::jmap::api::TransportError>>
        {
            if (eventName.empty() && eventId.empty() && eventData.empty())
            {
                co_return std::nullopt;
            }

            auto parsed = parseEventSourcePushMessage(
                stream.subscription(), stream.summary().lastState, eventName, eventId, eventData);
            const auto parsedEventName = eventName;
            const auto parsedEventId = eventId;
            const auto parsedEventData = eventData;
            resetEvent();

            auto outcome = co_await stream.accept(std::move(parsed));
            if (const auto* ping = std::get_if<PushStreamPing>(&outcome))
            {
                activity.setTimeout(pushActivityTimeout(ping->interval));
                qCDebug(logEventSource).noquote()
                    << "server ping interval" << ping->interval.count() << "seconds"
                    << activity.serverBaseUrl();
                co_return std::nullopt;
            }
            if (const auto* error = std::get_if<PushStreamProtocolFailure>(&outcome))
            {
                qWarning().noquote() << "State-change source invalid event payload"
                                     << QString::fromStdString(parsedEventName)
                                     << QString::fromStdString(parsedEventId)
                                     << summarizeBody(QByteArray::fromStdString(parsedEventData));
                co_return makeTransportError(
                    javelin::jmap::api::TransportErrorCode::ResponseDecodingFailed, error->message);
            }

            co_return std::nullopt;
        };

        while (true)
        {
            if (cancellation.isCancelled())
            {
                co_return stream.summary();
            }

            if (reply->error() != QNetworkReply::NoError && reply->isFinished())
            {
                qWarning().noquote()
                    << "State-change source network error" << reply->url().toString()
                    << reply->error() << reply->errorString()
                    << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
                    << summarizeBody(reply->readAll());
                co_return mapReplyError(*reply);
            }

            const auto currentStatusAttribute =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
            const int statusCode =
                currentStatusAttribute.isValid() ? currentStatusAttribute.toInt() : 0;
            if (!responseHeadersValidated && currentStatusAttribute.isValid())
            {
                if (statusCode < 200 || statusCode >= 300)
                {
                    const QByteArray responseBody = reply->readAll();
                    qWarning().noquote()
                        << "State-change source HTTP failure" << reply->url().toString()
                        << statusCode << summarizeBody(responseBody);
                    co_return makeTransportError(
                        javelin::jmap::api::TransportErrorCode::HttpFailure,
                        reply->errorString().toStdString(), statusCode);
                }

                const auto contentType =
                    reply->header(QNetworkRequest::ContentTypeHeader).toString();
                if (!contentType.startsWith(QStringLiteral("text/event-stream"),
                                            Qt::CaseInsensitive))
                {
                    co_return makeTransportError(
                        javelin::jmap::api::TransportErrorCode::ResponseDecodingFailed,
                        "Event-source response did not have a text/event-stream content type.",
                        statusCode);
                }
                responseHeadersValidated = true;
                if (!connectedReported)
                {
                    connectedReported = true;
                    activity.recordActivity();
                }
            }

            if (!reply->isFinished() && reply->bytesAvailable() == 0)
            {
                const bool ready = co_await qCoro(reply).waitForReadyRead(activity.timeout());
                if (!ready && !reply->isFinished())
                {
                    reply->abort();
                    qWarning().noquote()
                        << "State-change source timed out waiting for event-source activity"
                        << reply->url().toString();
                    co_return makeTransportError(
                        javelin::jmap::api::TransportErrorCode::NetworkFailure,
                        "Timed out waiting for event-source activity.");
                }

                if (!ready && reply->isFinished())
                {
                    const QByteArray chunk = reply->readAll();
                    if (!chunk.isEmpty())
                    {
                        activity.recordActivity();
                    }
                    pendingBuffer += chunk;
                }
                else if (ready)
                {
                    const QByteArray chunk = reply->readAll();
                    if (!chunk.isEmpty())
                    {
                        activity.recordActivity();
                    }
                    pendingBuffer += chunk;
                }
            }
            else
            {
                const QByteArray chunk = reply->readAll();
                if (!chunk.isEmpty())
                {
                    activity.recordActivity();
                }
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
                    if (const auto error = co_await finalizeEvent(); error.has_value())
                    {
                        co_return *error;
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

                if (const auto error = co_await finalizeEvent(); error.has_value())
                {
                    co_return *error;
                }

                if (stream.summary().updateCount > 0)
                {
                    co_return stream.summary();
                }

                qWarning().noquote()
                    << "State-change source closed before a state event was received"
                    << reply->url().toString() << statusCode << summarizeBody(pendingBuffer);
                co_return makeTransportError(
                    javelin::jmap::api::TransportErrorCode::NetworkFailure,
                    "Event-source connection closed before a state event was received.",
                    statusCode > 0 ? std::optional{statusCode} : std::nullopt);
            }
        }
    }

} // namespace javelin::jmap::sync
