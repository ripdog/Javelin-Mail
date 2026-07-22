#include "jmap/sync/PreferredStateChangeSource.h"

#include <QLoggingCategory>
#include <QString>
#include <QTimer>

#include <chrono>
#include <utility>
#include <variant>

namespace javelin::jmap::sync
{
    Q_LOGGING_CATEGORY(logPreferredStateChangeSource, "jmap.push.preferred")

    PreferredStateChangeSource::PreferredStateChangeSource(
        javelin::jmap::api::WebSocketFailureCooldowns& cooldowns, std::string webSocketUrl,
        std::unique_ptr<StateChangeSource> webSocketSource,
        std::unique_ptr<StateChangeSource> httpFallbackSource)
        : m_cooldowns(cooldowns), m_webSocketUrl(std::move(webSocketUrl)),
          m_webSocketSource(std::move(webSocketSource)),
          m_httpFallbackSource(std::move(httpFallbackSource))
    {
    }

    PreferredStateChangeSource::~PreferredStateChangeSource()
    {
        cancel();
    }

    void PreferredStateChangeSource::cancel()
    {
        if (m_webSocketSource != nullptr)
        {
            m_webSocketSource->cancel();
        }
        if (m_httpFallbackSource != nullptr)
        {
            m_httpFallbackSource->cancel();
        }
    }

    QCoro::Task<StateChangeSourceResult>
    PreferredStateChangeSource::consume(StateChangeSubscription subscription,
                                        StateChangeConsumer& consumer,
                                        StateChangeCancellation& cancellation)
    {
        while (!cancellation.isCancelled())
        {
            if (const auto retryDelay = m_cooldowns.retryDelay(m_webSocketUrl);
                retryDelay.has_value() && m_httpFallbackSource != nullptr)
            {
                qCInfo(logPreferredStateChangeSource)
                    << "using temporary HTTP state-change fallback";
                bool cooldownExpired = false;
                QTimer retryTimer;
                retryTimer.setSingleShot(true);
                retryTimer.setTimerType(Qt::PreciseTimer);
                QObject::connect(&retryTimer, &QTimer::timeout, &retryTimer,
                                 [this, &cooldownExpired]()
                                 {
                                     cooldownExpired = true;
                                     m_httpFallbackSource->cancel();
                                 });
                retryTimer.start(*retryDelay);
                auto fallbackResult =
                    co_await m_httpFallbackSource->consume(subscription, consumer, cancellation);
                retryTimer.stop();
                if (cooldownExpired && !cancellation.isCancelled())
                    continue;
                co_return fallbackResult;
            }

            const auto webSocketResult =
                co_await m_webSocketSource->consume(subscription, consumer, cancellation);
            if (std::holds_alternative<StateChangeStreamSummary>(webSocketResult))
            {
                m_cooldowns.recordSuccess(m_webSocketUrl);
                co_return webSocketResult;
            }

            const auto& transportError =
                std::get<javelin::jmap::api::TransportError>(webSocketResult);
            if (transportError.code == javelin::jmap::api::TransportErrorCode::Cancelled ||
                cancellation.isCancelled() || m_httpFallbackSource == nullptr)
            {
                co_return webSocketResult;
            }

            m_cooldowns.recordFailure(m_webSocketUrl);
            qCWarning(logPreferredStateChangeSource).noquote()
                << "WebSocket state changes unavailable; switching to HTTP for 15 minutes"
                << QString::fromStdString(transportError.message);
        }

        co_return javelin::jmap::api::TransportError{
            .code = javelin::jmap::api::TransportErrorCode::Cancelled,
            .message = "State-change source cancelled.",
        };
    }

} // namespace javelin::jmap::sync
