#include "app/DaemonBackgroundController.h"

#include "app/ApplicationErrorCoordinator.h"
#include "app/CalendarNotificationService.h"
#include "app/DaemonServices.h"
#include "app/DaemonTrayController.h"
#include "app/DeferredSendService.h"
#include "app/DesktopNotificationController.h"
#include "app/FullMailSyncService.h"
#include "app/LocalMaintenanceService.h"
#include "app/MailApplicationEventsPorts.h"
#include "app/MailApplicationService.h"
#include "app/MailIndexService.h"

#include <QDebug>
#include <QNetworkInformation>

#include <utility>

namespace javelin::app
{
    DaemonBackgroundController::DaemonBackgroundController(DaemonServices& services,
                                                           QObject* parent)
        : QObject(parent), m_services(services),
          m_notifications(std::make_unique<DesktopNotificationController>(this)),
          m_tray(std::make_unique<DaemonTrayController>(services.workScheduler(), this))
    {
        m_notificationRetryTimer.setSingleShot(true);
        m_notificationRetryTimer.setInterval(60000);
        connect(&m_notificationRetryTimer, &QTimer::timeout, this,
                &DaemonBackgroundController::retryMailNotifications);
    }

    DaemonBackgroundController::~DaemonBackgroundController()
    {
        stop();
    }

    void DaemonBackgroundController::start()
    {
        if (m_started)
            return;
        m_started = true;

        auto& mailService = m_services.mailService();
        connect(
            &mailService, &MailApplicationService::notificationRaised, this,
            [this](const QString& accountId, const QString& mailboxId, const QString& threadId,
                   const QString& emailId, const QString& mailboxName, const QString& title,
                   const QString& message, const QStringList& deliveredEmailIds)
            {
                if (m_notifications->notifyNewMail(accountId, mailboxId, threadId, emailId,
                                                   mailboxName, title, message))
                {
                    if (const auto error = m_services.mailService().markMailNotificationsDelivered(
                            accountId.toStdString(), mailboxId.toStdString(), deliveredEmailIds))
                        qWarning().noquote() << QStringLiteral("Record mail notification delivery:")
                                             << error->message;
                    return;
                }

                if (const auto error = m_services.mailService().releaseMailNotificationDispatches(
                        accountId.toStdString(), deliveredEmailIds))
                    qWarning().noquote()
                        << QStringLiteral("Release mail notification delivery:") << error->message;
                queueNotificationRetry(accountId);
            });
        connect(&mailService, &MailApplicationService::cacheCommitted, this,
                [this](MailCacheChange change)
                {
                    m_services.localMaintenanceService().requestReplay();
                    if (!change.optimisticProjection)
                        m_services.fullMailSyncService().requestCatchUp(
                            change.accountId.toStdString());
                    if (change.hasNewMail)
                        m_services.mailIndexService().requestIndex(change.accountId.toStdString());
                });
        connect(&m_services.errorCoordinator(), &ApplicationErrorCoordinator::incidentRaised, this,
                [this](const QString& connectionId, const QString&, const QString& title,
                       const QString& message, const bool persistent, const bool opensSettings)
                {
                    m_notifications->notifyError(connectionId, title, message, persistent,
                                                 opensSettings);
                });
        connect(m_notifications.get(), &DesktopNotificationController::notificationActivated, this,
                [this](const QString& accountId, const QString& mailboxId, const QString& threadId,
                       const QString& emailId, const QString& activationToken)
                {
                    Q_EMIT activationRequested(protocol::OpenMessageRoute{
                        .accountId = accountId,
                        .mailboxId = mailboxId,
                        .threadId = threadId,
                        .emailId = emailId,
                        .activationToken = activationToken,
                    });
                });
        connect(m_notifications.get(), &DesktopNotificationController::errorNotificationActivated,
                this,
                [this](const QString& connectionId, const QString& activationToken)
                {
                    Q_EMIT activationRequested(protocol::OpenSettingsRoute{
                        .connectionId = connectionId,
                        .activationToken = activationToken,
                    });
                });
        connect(&m_services.calendarNotificationService(),
                &CalendarNotificationService::reminderDue, this,
                [this](const QString& key, const QString& title, const QString& message)
                {
                    if (m_notifications->notifyCalendarEvent(key, title, message))
                        m_services.calendarNotificationService().deliveryAccepted(key);
                    else
                        m_services.calendarNotificationService().deliveryFailed(key);
                });
        connect(m_notifications.get(), &DesktopNotificationController::calendarNotificationAction,
                this,
                [this](const QString& key, const bool snooze)
                {
                    if (snooze)
                        m_services.calendarNotificationService().snooze(key);
                    else
                        m_services.calendarNotificationService().dismiss(key);
                });
        connect(&m_services.deferredSendService(), &DeferredSendService::undoableSendScheduled,
                m_notifications.get(), &DesktopNotificationController::notifyUndoableSend);
        connect(&m_services.deferredSendService(), &DeferredSendService::undoableSendWaiting,
                m_notifications.get(),
                [this](const QString& sendId, const QString& title, const QString& message)
                { m_notifications->notifyUndoableSend(sendId, title, message, 0); });
        connect(&m_services.deferredSendService(), &DeferredSendService::undoableSendClosed,
                m_notifications.get(),
                &DesktopNotificationController::closeUndoableSendNotification);
        connect(m_notifications.get(), &DesktopNotificationController::undoSendRequested, this,
                [this](const QString& sendId)
                { static_cast<void>(m_services.deferredSendService().cancelTargeted(sendId)); });
        connect(&m_services.deferredSendService(), &DeferredSendService::draftRestoreRequested,
                this,
                [this](const QString& accountId, const QString& draftEmailId,
                       const QString& composeSessionId)
                {
                    Q_EMIT activationRequested(protocol::RestoreDraftRoute{
                        .accountId = accountId,
                        .draftEmailId = draftEmailId,
                        .composeSessionId = composeSessionId,
                        .activationToken = {},
                    });
                });
        connect(&m_services.deferredSendService(), &DeferredSendService::sendFailed,
                m_notifications.get(),
                [this](const QString&, const QString& message)
                {
                    m_notifications->notifyError({}, QStringLiteral("Unable to send message"),
                                                 message, true, false);
                });
        connect(m_tray.get(), &DaemonTrayController::raiseGuiRequested, this,
                [this](const QString& activationToken)
                {
                    Q_EMIT activationRequested(
                        protocol::RaiseGuiRoute{.activationToken = activationToken});
                });
        connect(m_tray.get(), &DaemonTrayController::taskCenterRequested, this,
                [this](const QString& activationToken)
                {
                    Q_EMIT activationRequested(protocol::OpenTaskCenterRoute{
                        .activationToken = activationToken,
                    });
                });
        connect(m_tray.get(), &DaemonTrayController::refreshRequested, this,
                &DaemonBackgroundController::requestAccountRefresh);
        connect(m_tray.get(), &DaemonTrayController::quitRequested, this,
                &DaemonBackgroundController::shutdownRequested);

        if (const auto error = mailService.recoverMailNotificationDispatches())
            qWarning().noquote() << QStringLiteral("Recover mail notification delivery:")
                                 << error->message;
        m_services.deferredSendService().start();
        m_services.calendarNotificationService().start();
        setupNetworkReachability();
        if (!m_tray->start())
            qInfo() << QStringLiteral("Tray integration is unavailable; daemon services continue");
    }

    void DaemonBackgroundController::stop()
    {
        if (!m_started && !m_tray->isAvailable())
            return;
        m_notificationRetryTimer.stop();
        m_notificationRetryAccounts.clear();
        m_tray->stop();
        m_started = false;
    }

    void DaemonBackgroundController::setupNetworkReachability()
    {
        if (!QNetworkInformation::loadDefaultBackend())
        {
            qWarning() << QStringLiteral(
                "Network reachability backend unavailable; resume watchdog remains active");
            return;
        }

        auto* networkInformation = QNetworkInformation::instance();
        if (networkInformation == nullptr)
            return;

        connect(networkInformation, &QNetworkInformation::reachabilityChanged, this,
                [this](const QNetworkInformation::Reachability reachability)
                {
                    if (reachability != QNetworkInformation::Reachability::Online)
                        return;
                    qInfo() << QStringLiteral(
                        "Network became reachable; reconnecting account synchronization");
                    m_services.mailService().networkBecameReachable();
                });
    }

    void DaemonBackgroundController::retryMailNotifications()
    {
        const auto accounts = std::exchange(m_notificationRetryAccounts, {});
        for (const auto& accountId : accounts)
        {
            if (!m_services.mailService().requestAccountSynchronization(accountId.toStdString()))
                qWarning().noquote()
                    << QStringLiteral("Retry mail notification synchronization failed for")
                    << accountId;
        }
    }

    void DaemonBackgroundController::requestAccountRefresh()
    {
        for (const auto& [accountId, status] : m_services.mailService().accountStatuses())
        {
            static_cast<void>(status);
            if (!m_services.mailService().requestAccountSynchronization(accountId))
                qWarning().noquote()
                    << QStringLiteral("Tray refresh could not start synchronization for")
                    << QString::fromStdString(accountId);
        }
    }

    void DaemonBackgroundController::queueNotificationRetry(const QString& accountId)
    {
        m_notificationRetryAccounts.insert(accountId);
        if (!m_notificationRetryTimer.isActive())
            m_notificationRetryTimer.start();
    }
} // namespace javelin::app
