#include "app/OAuthRefreshSingleFlight.h"

#include <QCoroFuture>
#include <QCoroTask>

#include <utility>

namespace javelin::app
{
    OAuthRefreshSingleFlight::OAuthRefreshSingleFlight(QObject* parent) : QObject(parent)
    {
    }

    OAuthRefreshSingleFlight::~OAuthRefreshSingleFlight()
    {
        cancel();
    }

    QCoro::Task<OAuthRefreshOutcome> OAuthRefreshSingleFlight::run(QString connectionId,
                                                                   Operation operation)
    {
        if (const auto active = m_inFlight.find(connectionId); active != m_inFlight.end())
        {
            auto future = active->future;
            co_return co_await qCoro(future).result();
        }
        if (!operation)
            co_return OAuthRefreshOutcome{};

        const auto generation = ++m_generation;
        auto promise = std::make_shared<QPromise<OAuthRefreshOutcome>>();
        promise->start();
        auto future = promise->future();
        m_inFlight.insert(connectionId, InFlight{
                                            .generation = generation,
                                            .future = future,
                                            .promise = std::move(promise),
                                        });

        auto task = operation();
        QCoro::connect(std::move(task), this,
                       [this, connectionId, generation](OAuthRefreshOutcome outcome)
                       { complete(connectionId, generation, std::move(outcome)); });
        co_return co_await qCoro(future).result();
    }

    void OAuthRefreshSingleFlight::cancel()
    {
        QHash<QString, InFlight> active;
        active.swap(m_inFlight);
        for (auto& refresh : active)
        {
            refresh.promise->addResult(OAuthRefreshOutcome{});
            refresh.promise->finish();
        }
    }

    void OAuthRefreshSingleFlight::complete(const QString& connectionId, const quint64 generation,
                                            OAuthRefreshOutcome outcome)
    {
        const auto active = m_inFlight.find(connectionId);
        if (active == m_inFlight.end() || active->generation != generation)
            return;
        auto promise = active->promise;
        m_inFlight.erase(active);
        promise->addResult(std::move(outcome));
        promise->finish();
    }
} // namespace javelin::app
