#include "app/DesktopNotificationController.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDebug>
#include <QVariantMap>

namespace javelin::app
{

    namespace
    {
        constexpr auto notificationsService = "org.freedesktop.Notifications";
        constexpr auto notificationsPath = "/org/freedesktop/Notifications";
        constexpr auto notificationsInterface = "org.freedesktop.Notifications";
        constexpr auto defaultActionKey = "default";
        constexpr auto notificationIconName = "mail-unread";
        constexpr auto desktopEntryName = "javelinmail";
        constexpr auto defaultTimeoutMs = -1;
        constexpr auto urgencyNormal = 1;
        constexpr auto urgencyCritical = 2;
        constexpr auto dismissedByUserReason = 2U;
    } // namespace

    DesktopNotificationController::DesktopNotificationController(QObject* parent) : QObject(parent)
    {
        connectSignal("ActionInvoked", SLOT(onActionInvoked(uint, QString)));
        connectSignal("ActivationToken", SLOT(onActivationToken(uint, QString)));
        connectSignal("NotificationClosed", SLOT(onNotificationClosed(uint, uint)));
    }

    void DesktopNotificationController::notifyNewMail(const QString& accountId,
                                                      const QString& mailboxId,
                                                      const QString& threadId,
                                                      const QString& emailId,
                                                      const QString& mailboxName,
                                                      const QString& title, const QString& message)
    {
        QDBusInterface notifications{
            QString::fromLatin1(notificationsService), QString::fromLatin1(notificationsPath),
            QString::fromLatin1(notificationsInterface), QDBusConnection::sessionBus()};
        if (!notifications.isValid())
        {
            qWarning() << "Desktop notifications are unavailable on the session bus";
            return;
        }

        const QString summary = mailboxName.isEmpty() ? title : QStringLiteral("%1").arg(title);
        const QStringList actions = {
            QString::fromLatin1(defaultActionKey),
            QStringLiteral("Open"),
        };
        const auto reply = notifications.call(
            QStringLiteral("Notify"), QStringLiteral("Javelin Mail"), static_cast<uint>(0),
            QString::fromLatin1(notificationIconName), summary, message, actions,
            notificationHints(urgencyNormal), defaultTimeoutMs);
        const QDBusReply<uint> notificationReply{reply};
        if (!notificationReply.isValid())
        {
            qWarning().noquote() << "Failed to send desktop notification"
                                 << notificationReply.error().message();
            return;
        }

        const auto notificationId = notificationReply.value();
        m_trackedNotifications.insert_or_assign(notificationId, TrackedNotification{
                                                                    .accountId = accountId,
                                                                    .mailboxId = mailboxId,
                                                                    .threadId = threadId,
                                                                    .emailId = emailId,
                                                                    .activationToken = {},
                                                                    .connectionId = {},
                                                                    .calendarNotificationKey = {},
                                                                    .sendId = {},
                                                                    .opensSettings = false,
                                                                });
    }

    void DesktopNotificationController::notifyError(const QString& connectionId,
                                                    const QString& title, const QString& message,
                                                    const bool persistent, const bool opensSettings)
    {
        QDBusInterface notifications{
            QString::fromLatin1(notificationsService), QString::fromLatin1(notificationsPath),
            QString::fromLatin1(notificationsInterface), QDBusConnection::sessionBus()};
        if (!notifications.isValid())
        {
            qWarning() << "Desktop notifications are unavailable on the session bus";
            return;
        }

        const QStringList actions = opensSettings
                                        ? QStringList{QString::fromLatin1(defaultActionKey),
                                                      QStringLiteral("Open Settings")}
                                        : QStringList{};
        const auto reply = notifications.call(
            QStringLiteral("Notify"), QStringLiteral("Javelin Mail"), static_cast<uint>(0),
            QStringLiteral("dialog-warning"), title, message, actions,
            notificationHints(persistent ? urgencyCritical : urgencyNormal),
            persistent ? 0 : defaultTimeoutMs);
        const QDBusReply<uint> notificationReply{reply};
        if (!notificationReply.isValid())
        {
            qWarning().noquote() << "Failed to send error notification"
                                 << notificationReply.error().message();
            return;
        }

        m_trackedNotifications.insert_or_assign(
            notificationReply.value(), TrackedNotification{.accountId = {},
                                                           .mailboxId = {},
                                                           .threadId = {},
                                                           .emailId = {},
                                                           .activationToken = {},
                                                           .connectionId = connectionId,
                                                           .calendarNotificationKey = {},
                                                           .sendId = {},
                                                           .opensSettings = opensSettings});
    }

    void DesktopNotificationController::notifyCalendarEvent(const QString& key,
                                                            const QString& title,
                                                            const QString& message)
    {
        QDBusInterface notifications{
            QString::fromLatin1(notificationsService), QString::fromLatin1(notificationsPath),
            QString::fromLatin1(notificationsInterface), QDBusConnection::sessionBus()};
        if (!notifications.isValid())
        {
            qWarning() << "Desktop notifications are unavailable on the session bus";
            return;
        }
        const QStringList actions = {QStringLiteral("dismiss"), QStringLiteral("Dismiss"),
                                     QStringLiteral("snooze"), QStringLiteral("Snooze 5 min")};
        const QDBusReply<uint> reply{
            notifications.call(QStringLiteral("Notify"), QStringLiteral("Javelin Mail"),
                               static_cast<uint>(0), QStringLiteral("javelinmail"), title, message,
                               actions, notificationHints(urgencyNormal, false), 0)};
        if (!reply.isValid())
        {
            qWarning().noquote() << "Failed to send calendar notification"
                                 << reply.error().message();
            return;
        }
        m_trackedNotifications.insert_or_assign(reply.value(),
                                                TrackedNotification{.accountId = {},
                                                                    .mailboxId = {},
                                                                    .threadId = {},
                                                                    .emailId = {},
                                                                    .activationToken = {},
                                                                    .connectionId = {},
                                                                    .calendarNotificationKey = key,
                                                                    .sendId = {},
                                                                    .opensSettings = false});
    }

    void DesktopNotificationController::notifyUndoableSend(const QString& sendId,
                                                           const QString& title,
                                                           const QString& message,
                                                           const int timeoutMs)
    {
        closeUndoableSendNotification(sendId);
        QDBusInterface notifications{
            QString::fromLatin1(notificationsService), QString::fromLatin1(notificationsPath),
            QString::fromLatin1(notificationsInterface), QDBusConnection::sessionBus()};
        if (!notifications.isValid())
            return;
        const QStringList actions = {QStringLiteral("undo-send"), QStringLiteral("Undo Send")};
        const QDBusReply<uint> reply{
            notifications.call(QStringLiteral("Notify"), QStringLiteral("Javelin Mail"),
                               static_cast<uint>(0), QStringLiteral("mail-send"), title, message,
                               actions, notificationHints(urgencyNormal), timeoutMs)};
        if (!reply.isValid())
            return;
        const auto notificationId = reply.value();
        m_sendNotificationIds.insert(sendId, notificationId);
        m_trackedNotifications.insert_or_assign(notificationId,
                                                TrackedNotification{.accountId = {},
                                                                    .mailboxId = {},
                                                                    .threadId = {},
                                                                    .emailId = {},
                                                                    .activationToken = {},
                                                                    .connectionId = {},
                                                                    .calendarNotificationKey = {},
                                                                    .sendId = sendId,
                                                                    .opensSettings = false});
    }

    void DesktopNotificationController::closeUndoableSendNotification(const QString& sendId)
    {
        const auto found = m_sendNotificationIds.find(sendId);
        if (found == m_sendNotificationIds.end())
            return;
        QDBusInterface notifications{
            QString::fromLatin1(notificationsService), QString::fromLatin1(notificationsPath),
            QString::fromLatin1(notificationsInterface), QDBusConnection::sessionBus()};
        if (notifications.isValid())
            static_cast<void>(
                notifications.call(QStringLiteral("CloseNotification"), found.value()));
        untrackNotification(found.value());
    }

    void DesktopNotificationController::onActionInvoked(const uint notificationId,
                                                        const QString& actionKey)
    {
        const auto it = m_trackedNotifications.find(notificationId);
        if (it == m_trackedNotifications.end())
        {
            return;
        }

        if (!it->second.calendarNotificationKey.isEmpty())
        {
            if (actionKey == QStringLiteral("dismiss") || actionKey == QStringLiteral("snooze"))
                Q_EMIT calendarNotificationAction(it->second.calendarNotificationKey,
                                                  actionKey == QStringLiteral("snooze"));
            untrackNotification(notificationId);
            return;
        }
        if (!it->second.sendId.isEmpty())
        {
            if (actionKey == QStringLiteral("undo-send"))
                Q_EMIT undoSendRequested(it->second.sendId);
            untrackNotification(notificationId);
            return;
        }
        if (actionKey != QString::fromLatin1(defaultActionKey))
            return;

        if (it->second.opensSettings)
        {
            Q_EMIT errorNotificationActivated(it->second.connectionId, it->second.activationToken);
            return;
        }

        Q_EMIT notificationActivated(it->second.accountId, it->second.mailboxId,
                                     it->second.threadId, it->second.emailId,
                                     it->second.activationToken);
    }

    void DesktopNotificationController::onActivationToken(const uint notificationId,
                                                          const QString& activationToken)
    {
        const auto it = m_trackedNotifications.find(notificationId);
        if (it == m_trackedNotifications.end())
        {
            return;
        }

        it->second.activationToken = activationToken;
    }

    void DesktopNotificationController::onNotificationClosed(const uint notificationId,
                                                             const uint reason)
    {
        const auto found = m_trackedNotifications.find(notificationId);
        if (found != m_trackedNotifications.end() &&
            !found->second.calendarNotificationKey.isEmpty() && reason == dismissedByUserReason)
            Q_EMIT calendarNotificationAction(found->second.calendarNotificationKey, false);
        untrackNotification(notificationId);
    }

    bool DesktopNotificationController::connectSignal(const char* signalName, const char* slotName)
    {
        const bool connected = QDBusConnection::sessionBus().connect(
            QString::fromLatin1(notificationsService), QString::fromLatin1(notificationsPath),
            QString::fromLatin1(notificationsInterface), QString::fromLatin1(signalName), this,
            slotName);
        if (!connected)
        {
            qWarning() << "Failed to connect desktop notification signal" << signalName;
        }
        return connected;
    }

    QVariantMap
    DesktopNotificationController::notificationHints(const int urgency,
                                                     const bool activatesApplication) const
    {
        QVariantMap hints;
        if (activatesApplication)
            hints.insert(QStringLiteral("desktop-entry"), QString::fromLatin1(desktopEntryName));
        hints.insert(QStringLiteral("urgency"), urgency);
        hints.insert(QStringLiteral("sender-pid"),
                     static_cast<qlonglong>(QCoreApplication::applicationPid()));
        return hints;
    }

    void DesktopNotificationController::untrackNotification(const uint notificationId)
    {
        const auto found = m_trackedNotifications.find(notificationId);
        if (found != m_trackedNotifications.end() && !found->second.sendId.isEmpty())
            m_sendNotificationIds.remove(found->second.sendId);
        m_trackedNotifications.erase(notificationId);
    }

} // namespace javelin::app
