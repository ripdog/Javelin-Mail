#pragma once

#include "protocol/ActivationContract.h"

#include <QObject>
#include <QSet>
#include <QTimer>

#include <memory>

class QNetworkInformation;

namespace javelin::app
{
    class DaemonServices;
    class DaemonTrayController;
    class DesktopNotificationController;

    class DaemonBackgroundController final : public QObject
    {
        Q_OBJECT

      public:
        explicit DaemonBackgroundController(DaemonServices& services, QObject* parent = nullptr);
        DaemonBackgroundController(DaemonServices& services,
                                   std::unique_ptr<DesktopNotificationController> notifications,
                                   QObject* parent = nullptr);
        ~DaemonBackgroundController() override;

        DaemonBackgroundController(const DaemonBackgroundController&) = delete;
        DaemonBackgroundController& operator=(const DaemonBackgroundController&) = delete;
        DaemonBackgroundController(DaemonBackgroundController&&) = delete;
        DaemonBackgroundController& operator=(DaemonBackgroundController&&) = delete;

        void start();
        void stop();

      Q_SIGNALS:
        void activationRequested(javelin::protocol::ActivationRoute route);
        void shutdownRequested();

      private:
        void setupNetworkReachability();
        void retryMailNotifications();
        void refreshTrayUnreadCount();
        void queueNotificationRetry(const QString& accountId);

        DaemonServices& m_services;
        std::unique_ptr<DesktopNotificationController> m_notifications;
        std::unique_ptr<DaemonTrayController> m_tray;
        QTimer m_notificationRetryTimer;
        QSet<QString> m_notificationRetryAccounts;
        bool m_started = false;
    };
} // namespace javelin::app
