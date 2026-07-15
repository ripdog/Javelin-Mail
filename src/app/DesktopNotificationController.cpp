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
                                                           .opensSettings = opensSettings});
    }

    void DesktopNotificationController::onActionInvoked(const uint notificationId,
                                                        const QString& actionKey)
    {
        if (actionKey != QString::fromLatin1(defaultActionKey))
        {
            return;
        }

        const auto it = m_trackedNotifications.find(notificationId);
        if (it == m_trackedNotifications.end())
        {
            return;
        }

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
        static_cast<void>(reason);
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

    QVariantMap DesktopNotificationController::notificationHints(const int urgency) const
    {
        QVariantMap hints;
        hints.insert(QStringLiteral("desktop-entry"), QString::fromLatin1(desktopEntryName));
        hints.insert(QStringLiteral("urgency"), urgency);
        hints.insert(QStringLiteral("sender-pid"),
                     static_cast<qlonglong>(QCoreApplication::applicationPid()));
        return hints;
    }

    void DesktopNotificationController::untrackNotification(const uint notificationId)
    {
        m_trackedNotifications.erase(notificationId);
    }

} // namespace javelin::app
