#include "jmap/sync/PreferredStateChangeSource.h"

#include "jmap/cache/JmapTransportPreferenceRepository.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QString>

#include <chrono>
#include <optional>
#include <utility>
#include <variant>

namespace javelin::jmap::sync
{
    Q_LOGGING_CATEGORY(logPreferredStateChangeSource, "jmap.push.preferred")

    namespace
    {
        constexpr auto fallbackRetryInterval = std::chrono::hours{6};
    }

    PreferredStateChangeSource::PreferredStateChangeSource(
        javelin::jmap::cache::DatabaseConnection& databaseConnection, std::string accountId,
        std::string webSocketUrl, std::unique_ptr<StateChangeSource> webSocketSource,
        std::unique_ptr<StateChangeSource> httpFallbackSource)
        : m_databaseConnection(databaseConnection), m_accountId(std::move(accountId)),
          m_webSocketUrl(std::move(webSocketUrl)),
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
        javelin::jmap::cache::JmapTransportPreferenceRepository preferences{
            m_databaseConnection};
        bool shouldAttemptWebSocket = true;
        std::string ownerAccountId = m_accountId;
        const auto targetResult = preferences.resolve(m_accountId);
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&targetResult))
        {
            qCWarning(logPreferredStateChangeSource).noquote()
                << "could not resolve transport preference" << error->message;
        }
        else if (const auto& target =
                     std::get<std::optional<javelin::jmap::cache::JmapTransportTarget>>(
                         targetResult);
                 target.has_value() && target->webSocketUrl == m_webSocketUrl)
        {
            ownerAccountId = target->ownerAccountId;
            shouldAttemptWebSocket =
                target->shouldAttemptWebSocket(QDateTime::currentDateTimeUtc());
        }

        if (!shouldAttemptWebSocket && m_httpFallbackSource != nullptr)
        {
            qCInfo(logPreferredStateChangeSource)
                << "using remembered HTTP state-change fallback";
            co_return co_await m_httpFallbackSource->consume(std::move(subscription), consumer,
                                                             cancellation);
        }

        const auto webSocketResult =
            co_await m_webSocketSource->consume(subscription, consumer, cancellation);
        if (std::holds_alternative<StateChangeStreamSummary>(webSocketResult))
        {
            if (const auto error =
                    preferences.markWebSocketAvailable(ownerAccountId, m_webSocketUrl))
            {
                qCWarning(logPreferredStateChangeSource).noquote()
                    << "could not persist working WebSocket state" << error->message;
            }
            co_return webSocketResult;
        }

        const auto& transportError =
            std::get<javelin::jmap::api::TransportError>(webSocketResult);
        if (transportError.code == javelin::jmap::api::TransportErrorCode::Cancelled ||
            cancellation.isCancelled() || m_httpFallbackSource == nullptr)
        {
            co_return webSocketResult;
        }

        const auto retryAfter =
            QDateTime::currentDateTimeUtc().addSecs(
                std::chrono::duration_cast<std::chrono::seconds>(fallbackRetryInterval).count());
        if (const auto error = preferences.markHttpFallback(
                ownerAccountId, m_webSocketUrl, retryAfter,
                QString::fromStdString(transportError.message)))
        {
            qCWarning(logPreferredStateChangeSource).noquote()
                << "could not persist HTTP state-change fallback" << error->message;
        }
        qCWarning(logPreferredStateChangeSource).noquote()
            << "WebSocket state changes unavailable; switching to HTTP until"
            << retryAfter.toString(Qt::ISODateWithMs)
            << QString::fromStdString(transportError.message);

        co_return co_await m_httpFallbackSource->consume(std::move(subscription), consumer,
                                                         cancellation);
    }

} // namespace javelin::jmap::sync
