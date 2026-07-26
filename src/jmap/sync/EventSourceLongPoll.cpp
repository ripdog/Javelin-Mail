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
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopeGuard>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <charconv>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace javelin::jmap::sync
{
    Q_LOGGING_CATEGORY(logEventSource, "jmap.push.eventsource")

    struct RawStateChange
    {
        std::optional<std::string> type;
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> changed;
    };

    struct RawPing
    {
        std::optional<std::variant<unsigned int, std::string>> interval;
    };

} // namespace javelin::jmap::sync

template <> struct glz::meta<javelin::jmap::sync::RawStateChange>
{
    using T = javelin::jmap::sync::RawStateChange;

    static constexpr auto value = glz::object("type", &T::type, "changed", &T::changed);
};

template <> struct glz::meta<javelin::jmap::sync::RawPing>
{
    using T = javelin::jmap::sync::RawPing;

    static constexpr auto value = glz::object("interval", &T::interval);
};

namespace javelin::jmap::sync
{

    EventSourcePingIntervalResult parseEventSourcePingInterval(const std::string_view eventData)
    {
        RawPing ping;
        std::string buffer{eventData};
        if (const auto readError =
                glz::read<glz::opts{.error_on_unknown_keys = false}>(ping, buffer))
            return std::string{glz::format_error(readError, buffer)};
        if (!ping.interval.has_value())
            return std::optional<std::chrono::seconds>{std::nullopt};

        unsigned int interval = 0;
        if (const auto* numeric = std::get_if<unsigned int>(&*ping.interval))
        {
            interval = *numeric;
        }
        else
        {
            const auto& text = std::get<std::string>(*ping.interval);
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), interval);
            if (error != std::errc{} || end != text.data() + text.size())
                return std::string{"Ping interval string is not an unsigned decimal integer."};
        }

        if (interval > 1000 && interval % 1000 == 0)
        {
            // Stalwart currently reports this RFC 8620 value in milliseconds.
            interval /= 1000;
        }
        return interval > 0 ? std::optional<std::chrono::seconds>{std::chrono::seconds{interval}}
                            : std::optional<std::chrono::seconds>{std::nullopt};
    }

    namespace
    {
        enum class ParsedEventStatus
        {
            NeedMoreData,
            Ignored,
            Parsed,
            Invalid,
        };

        constexpr auto requestedPingInterval = std::chrono::seconds{30};
        constexpr auto eventSourcePingGrace = std::chrono::seconds{15};
        constexpr auto maximumAcceptedPingInterval = std::chrono::seconds{300};
        constexpr auto maximumEventSourceIdleTimeout = std::chrono::seconds{350};
        constexpr auto defaultEventSourceIdleTimeout = maximumEventSourceIdleTimeout;

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
            std::optional<StateChangeEvent> response;
            std::optional<std::string> errorMessage;
            std::optional<std::chrono::seconds> pingInterval;
        };

        using ParsedStreamEvent =
            std::variant<StateChangeEvent, javelin::jmap::api::TransportError>;

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
                                 .arg(requestedPingInterval.count()));
            expanded.replace(QStringLiteral("{types}"), encodeTemplateValue(types));
            expanded.replace(QStringLiteral("{closeafter}"), QStringLiteral("no"));
            expanded.replace(QStringLiteral("{ping}"),
                             QString::number(requestedPingInterval.count()));

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
                                   QString::number(requestedPingInterval.count()));
                url.setQuery(query);
            }

            return url;
        }

        [[nodiscard]] ParsedEvent parseStateEvent(const StateChangeSubscription& subscription,
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
                    .pingInterval = std::nullopt,
                };
            }

            if (eventName == "ping")
            {
                const auto pingIntervalResult = parseEventSourcePingInterval(eventData);
                if (const auto* error = std::get_if<std::string>(&pingIntervalResult))
                {
                    return ParsedEvent{
                        .status = ParsedEventStatus::Invalid,
                        .response = std::nullopt,
                        .errorMessage = *error,
                        .pingInterval = std::nullopt,
                    };
                }

                return ParsedEvent{
                    .status = ParsedEventStatus::Ignored,
                    .response = std::nullopt,
                    .errorMessage = std::nullopt,
                    .pingInterval =
                        std::get<std::optional<std::chrono::seconds>>(pingIntervalResult),
                };
            }

            if (eventName != "state")
            {
                return ParsedEvent{
                    .status = ParsedEventStatus::Ignored,
                    .response = std::nullopt,
                    .errorMessage = std::nullopt,
                    .pingInterval = std::nullopt,
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
                    .pingInterval = std::nullopt,
                };
            }

            if (stateChange.type == std::optional<std::string>{"connect"})
            {
                return ParsedEvent{
                    .status = ParsedEventStatus::Parsed,
                    .response =
                        StateChangeEvent{
                            .newState =
                                eventId.empty() ? std::string{fallbackState} : std::string{eventId},
                            .changedTypes = {},
                            .changedStates = {},
                            .notifyConsumer = false,
                        },
                    .errorMessage = std::nullopt,
                    .pingInterval = std::nullopt,
                };
            }

            if (stateChange.type.has_value() && *stateChange.type != "StateChange")
            {
                return ParsedEvent{
                    .status = ParsedEventStatus::Invalid,
                    .response = std::nullopt,
                    .errorMessage = std::string{"Expected a StateChange event payload."},
                    .pingInterval = std::nullopt,
                };
            }

            auto changedStates = subscribedStateChanges(subscription, stateChange.changed);
            if (changedStates.empty())
            {
                return ParsedEvent{
                    .status = ParsedEventStatus::Ignored,
                    .response = std::nullopt,
                    .errorMessage = std::nullopt,
                    .pingInterval = std::nullopt,
                };
            }

            std::vector<std::string> changedTypes;
            changedTypes.reserve(changedStates.size());
            for (const auto& [typeName, state] : changedStates)
            {
                static_cast<void>(state);
                changedTypes.push_back(typeName);
            }

            return ParsedEvent{
                .status = ParsedEventStatus::Parsed,
                .response =
                    StateChangeEvent{
                        .newState =
                            eventId.empty() ? std::string{fallbackState} : std::string{eventId},
                        .changedTypes = std::move(changedTypes),
                        .changedStates = std::move(changedStates),
                    },
                .errorMessage = std::nullopt,
                .pingInterval = std::nullopt,
            };
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

    void EventSourceStateChangeSource::reportConnectedActivity() const
    {
        if (m_statusCallback)
        {
            m_statusCallback(StateChangeConnectionStatus::Connected);
        }
    }

    void EventSourceStateChangeSource::cancel()
    {
        auto* activeReply = m_activeReply.data();
        if (activeReply == nullptr)
        {
            return;
        }

        qInfo().noquote() << "State-change source aborting active event-source request"
                          << activeReply->url().toString();
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
                         [this, &connectedReported]()
                         {
                             if (!connectedReported)
                             {
                                 connectedReported = true;
                                 reportConnectedActivity();
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

        StateChangeStreamSummary streamSummary{
            .lastState = subscription.lastState,
            .updateCount = 0,
        };
        auto activityTimeout = defaultEventSourceIdleTimeout;
        bool responseHeadersValidated = false;

        const auto finalizeEvent = [&]() -> std::optional<ParsedStreamEvent>
        {
            if (eventName.empty() && eventId.empty() && eventData.empty())
            {
                return std::nullopt;
            }

            const auto parsed = parseStateEvent(subscription, subscription.lastState, eventName,
                                                eventId, eventData);
            if (parsed.pingInterval.has_value())
            {
                const auto effectivePingInterval =
                    std::min(*parsed.pingInterval, maximumAcceptedPingInterval);
                activityTimeout = std::min(effectivePingInterval * 2 + eventSourcePingGrace,
                                           maximumEventSourceIdleTimeout);
                qCDebug(logEventSource).noquote()
                    << "server ping interval" << parsed.pingInterval->count()
                    << "seconds; effective interval" << effectivePingInterval.count()
                    << "seconds; activity timeout" << activityTimeout.count() << "seconds";
            }
            if (eventName == "ping")
                qCDebug(logEventSource).noquote() << "ping received";
            else
                qCInfo(logEventSource).noquote()
                    << "event" << QString::fromStdString(eventName) << "id"
                    << QString::fromStdString(eventId) << "status"
                    << parsedEventStatusName(parsed.status);
            if (parsed.status == ParsedEventStatus::Invalid)
            {
                qWarning().noquote()
                    << "State-change source invalid event payload"
                    << QString::fromStdString(eventName) << QString::fromStdString(eventId)
                    << summarizeBody(QByteArray::fromStdString(eventData));
            }
            resetEvent();

            switch (parsed.status)
            {
            case ParsedEventStatus::NeedMoreData:
            case ParsedEventStatus::Ignored:
                return std::nullopt;
            case ParsedEventStatus::Parsed:
                if (parsed.response.has_value() && !parsed.response->notifyConsumer)
                {
                    subscription.lastState = parsed.response->newState;
                    streamSummary.lastState = parsed.response->newState;
                    return std::nullopt;
                }
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
            if (cancellation.isCancelled())
            {
                co_return streamSummary;
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
                    reportConnectedActivity();
                }
            }

            if (!reply->isFinished() && reply->bytesAvailable() == 0)
            {
                const bool ready = co_await qCoro(reply).waitForReadyRead(activityTimeout);
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
                    qCDebug(logEventSource).noquote()
                        << "raw event-source bytes" << reply->url().toString() << chunk.size()
                        << summarizeBody(chunk);
                    if (!chunk.isEmpty())
                    {
                        reportConnectedActivity();
                    }
                    pendingBuffer += chunk;
                }
                else if (ready)
                {
                    const QByteArray chunk = reply->readAll();
                    qCDebug(logEventSource).noquote()
                        << "raw event-source bytes" << reply->url().toString() << chunk.size()
                        << summarizeBody(chunk);
                    if (!chunk.isEmpty())
                    {
                        reportConnectedActivity();
                    }
                    pendingBuffer += chunk;
                }
            }
            else
            {
                const QByteArray chunk = reply->readAll();
                qCDebug(logEventSource).noquote()
                    << "raw event-source bytes" << reply->url().toString() << chunk.size()
                    << summarizeBody(chunk);
                if (!chunk.isEmpty())
                {
                    reportConnectedActivity();
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
                    if (const auto parsed = finalizeEvent(); parsed.has_value())
                    {
                        if (const auto* error =
                                std::get_if<javelin::jmap::api::TransportError>(&*parsed))
                        {
                            co_return *error;
                        }

                        auto event = std::get<StateChangeEvent>(*parsed);
                        subscription.lastState = event.newState;
                        streamSummary.lastState = event.newState;
                        ++streamSummary.updateCount;
                        co_await consumer.onStateChange(std::move(event));
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
                    if (const auto* error =
                            std::get_if<javelin::jmap::api::TransportError>(&*parsed))
                    {
                        co_return *error;
                    }

                    auto event = std::get<StateChangeEvent>(*parsed);
                    subscription.lastState = event.newState;
                    streamSummary.lastState = event.newState;
                    ++streamSummary.updateCount;
                    co_await consumer.onStateChange(std::move(event));
                    co_return streamSummary;
                }

                if (streamSummary.updateCount > 0)
                {
                    co_return streamSummary;
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
