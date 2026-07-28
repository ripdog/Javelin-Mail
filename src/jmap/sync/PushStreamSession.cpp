#include "jmap/sync/PushStreamSession.h"

#include <QLoggingCategory>
#include <QStringList>

#include <utility>

Q_LOGGING_CATEGORY(logPushStream, "jmap.push")

namespace javelin::jmap::sync
{
    PushStreamSession::PushStreamSession(StateChangeSubscription subscription,
                                         StateChangeConsumer& consumer)
        : m_subscription(std::move(subscription)), m_consumer(consumer),
          m_summary{.lastState = m_subscription.lastState, .updateCount = 0}
    {
    }

    const StateChangeSubscription& PushStreamSession::subscription() const
    {
        return m_subscription;
    }

    const StateChangeStreamSummary& PushStreamSession::summary() const
    {
        return m_summary;
    }

    QCoro::Task<PushStreamOutcome> PushStreamSession::accept(PushMessage message)
    {
        if (std::holds_alternative<PushMessageIgnored>(message))
            co_return PushStreamIgnored{};
        if (const auto* ping = std::get_if<PushPing>(&message))
            co_return PushStreamPing{.interval = ping->interval};
        if (auto* error = std::get_if<PushProtocolError>(&message))
            co_return PushStreamProtocolFailure{.message = std::move(error->message)};

        auto event = std::get<StateChangeEvent>(std::move(message));
        m_subscription.lastState = event.newState;
        m_summary.lastState = event.newState;
        QStringList changedDomains;
        for (const auto& [accountId, states] : event.changedStates)
        {
            for (const auto& [type, state] : states)
            {
                static_cast<void>(state);
                changedDomains.append(QStringLiteral("%1:%2").arg(QString::fromStdString(accountId),
                                                                  QString::fromStdString(type)));
            }
        }
        changedDomains.sort();
        if (!changedDomains.isEmpty())
            qCInfo(logPushStream).noquote()
                << "state change" << changedDomains.join(QLatin1Char(','));
        if (!event.notifyConsumer)
            co_return PushStreamIgnored{};

        ++m_summary.updateCount;
        co_await m_consumer.onStateChange(std::move(event));
        co_return PushStreamIgnored{};
    }

} // namespace javelin::jmap::sync
