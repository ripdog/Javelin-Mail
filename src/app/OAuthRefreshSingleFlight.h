#pragma once

#include <QCoroTask>

#include <QFuture>
#include <QHash>
#include <QObject>
#include <QPromise>
#include <QString>

#include <functional>
#include <memory>

namespace javelin::app
{
    struct OAuthRefreshOutcome
    {
        bool succeeded = false;
        QString accessToken;
    };

    class OAuthRefreshSingleFlight final : public QObject
    {
      public:
        using Operation = std::function<QCoro::Task<OAuthRefreshOutcome>()>;

        explicit OAuthRefreshSingleFlight(QObject* parent = nullptr);
        ~OAuthRefreshSingleFlight() override;

        [[nodiscard]] QCoro::Task<OAuthRefreshOutcome> run(QString connectionId,
                                                           Operation operation);
        void cancel();

      private:
        struct InFlight
        {
            quint64 generation = 0;
            QFuture<OAuthRefreshOutcome> future;
            std::shared_ptr<QPromise<OAuthRefreshOutcome>> promise;
        };

        void complete(const QString& connectionId, quint64 generation, OAuthRefreshOutcome outcome);

        QHash<QString, InFlight> m_inFlight;
        quint64 m_generation = 0;
    };
} // namespace javelin::app
