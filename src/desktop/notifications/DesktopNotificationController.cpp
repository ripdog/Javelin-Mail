#include "desktop/notifications/DesktopNotificationController.h"

#include <KLocalizedString>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDebug>
#include <QSet>
#include <QVariantMap>

#include <utility>
#include <vector>

namespace javelin::app
{

    namespace
    {
        constexpr auto notificationsService = "org.freedesktop.Notifications";
        constexpr auto notificationsPath = "/org/freedesktop/Notifications";
        constexpr auto notificationsInterface = "org.freedesktop.Notifications";
        constexpr auto defaultActionKey = "default";
        constexpr auto archiveActionKey = "archive";
        constexpr auto markReadActionKey = "mark-read";
        constexpr auto replyActionKey = "reply";
        constexpr auto notificationIconName = "mail-unread";
        constexpr auto desktopEntryName = "javelinmail";
        constexpr auto defaultTimeoutMs = -1;
        constexpr auto urgencyNormal = 1;
        constexpr auto urgencyCritical = 2;
        constexpr auto dismissedByUserReason = 2U;
        constexpr auto undoActionPrefix = "undo-send:";

        [[nodiscard]] QString undoActionKey(const QString& sendId)
        {
            return QString::fromLatin1(undoActionPrefix) + sendId;
        }

        [[nodiscard]] std::optional<QString> sendIdFromUndoAction(const QString& actionKey)
        {
            const auto prefix = QString::fromLatin1(undoActionPrefix);
            if (!actionKey.startsWith(prefix))
                return std::nullopt;
            const auto sendId = actionKey.sliced(prefix.size());
            if (sendId.isEmpty() || sendId.contains(QLatin1Char(':')))
                return std::nullopt;
            return sendId;
        }

        [[nodiscard]] DesktopNotificationCloseReason closeReason(const uint reason)
        {
            switch (reason)
            {
            case 1:
                return DesktopNotificationCloseReason::Expired;
            case 2:
                return DesktopNotificationCloseReason::DismissedByUser;
            case 3:
                return DesktopNotificationCloseReason::ClosedByApplication;
            case 4:
            default:
                return DesktopNotificationCloseReason::Undefined;
            }
        }

        class FreedesktopNotificationTransport final : public DesktopNotificationTransport
        {
          public:
            std::variant<uint, QString> send(const QString& icon, const QString& summary,
                                             const QString& message, const QStringList& actions,
                                             const QVariantMap& hints, const int timeoutMs) override
            {
                QDBusInterface notifications{QString::fromLatin1(notificationsService),
                                             QString::fromLatin1(notificationsPath),
                                             QString::fromLatin1(notificationsInterface),
                                             QDBusConnection::sessionBus()};
                if (!notifications.isValid())
                    return i18n("Desktop notifications are unavailable on the session bus");

                const QDBusReply<uint> reply{notifications.call(
                    QStringLiteral("Notify"), i18n("Javelin Mail"), static_cast<uint>(0), icon,
                    summary, message, actions, hints, timeoutMs)};
                if (!reply.isValid())
                    return reply.error().message();
                return reply.value();
            }

            [[nodiscard]] bool supportsActions() const override
            {
                QDBusInterface notifications{QString::fromLatin1(notificationsService),
                                             QString::fromLatin1(notificationsPath),
                                             QString::fromLatin1(notificationsInterface),
                                             QDBusConnection::sessionBus()};
                if (!notifications.isValid())
                    return false;

                const QDBusReply<QStringList> reply{
                    notifications.call(QStringLiteral("GetCapabilities"))};
                return reply.isValid() && reply.value().contains(QStringLiteral("actions"));
            }

            void close(const uint notificationId) override
            {
                QDBusInterface notifications{QString::fromLatin1(notificationsService),
                                             QString::fromLatin1(notificationsPath),
                                             QString::fromLatin1(notificationsInterface),
                                             QDBusConnection::sessionBus()};
                if (notifications.isValid())
                    static_cast<void>(
                        notifications.call(QStringLiteral("CloseNotification"), notificationId));
            }
        };
    } // namespace

    DesktopNotificationController::DesktopNotificationController(QObject* parent)
        : DesktopNotificationController(std::make_unique<FreedesktopNotificationTransport>(), true,
                                        parent)
    {
    }

    DesktopNotificationController::DesktopNotificationController(
        std::unique_ptr<DesktopNotificationTransport> transport, const bool connectSignals,
        QObject* parent)
        : QObject(parent), m_transport(std::move(transport))
    {
        if (!connectSignals)
            return;
        m_actionInvokedConnected =
            connectSignal("ActionInvoked", SLOT(onActionInvoked(uint, QString)));
        connectSignal("ActivationToken", SLOT(onActivationToken(uint, QString)));
        m_notificationClosedConnected =
            connectSignal("NotificationClosed", SLOT(onNotificationClosed(uint, uint)));
        m_notificationServiceWatcher = std::make_unique<QDBusServiceWatcher>(
            QString::fromLatin1(notificationsService), QDBusConnection::sessionBus(),
            QDBusServiceWatcher::WatchForUnregistration, this);
        connect(m_notificationServiceWatcher.get(), &QDBusServiceWatcher::serviceUnregistered, this,
                &DesktopNotificationController::onNotificationServiceUnregistered);
    }

    DesktopNotificationController::DesktopNotificationController(
        std::unique_ptr<DesktopNotificationTransport> transport, const bool connectSignals,
        const bool signalSubscriptionsConnected, QObject* parent)
        : DesktopNotificationController(std::move(transport), connectSignals, parent)
    {
        if (!connectSignals)
        {
            m_actionInvokedConnected = signalSubscriptionsConnected;
            m_notificationClosedConnected = signalSubscriptionsConnected;
        }
    }

    DesktopNotificationController::~DesktopNotificationController() = default;

    bool DesktopNotificationController::notifyNewMail(const QString& accountId,
                                                      const QString& mailboxId,
                                                      const QString& threadId,
                                                      const QString& emailId,
                                                      const QString& mailboxName,
                                                      const QString& title, const QString& message)
    {
        const QString summary = mailboxName.isEmpty() ? title : QStringLiteral("%1").arg(title);
        QStringList actions;
        if (m_actionInvokedConnected && transportSupportsActions())
        {
            actions = {
                QString::fromLatin1(defaultActionKey),
                i18nc("@action:button desktop notification", "Open"),
                QString::fromLatin1(archiveActionKey),
                i18nc("@action:button desktop notification", "Archive"),
                QString::fromLatin1(markReadActionKey),
                i18nc("@action:button desktop notification", "Mark Read"),
                QString::fromLatin1(replyActionKey),
                i18nc("@action:button desktop notification", "Reply"),
            };
        }
        // Plasma requests one activation token for every action using the notification-wide
        // desktop-entry hint. Archive and Mark Read are daemon-only, so associating this mixed
        // action set with the GUI would leave startup feedback waiting for a window that will
        // never open. Open and Reply still receive a usable token without an application id.
        const auto sent = m_transport->send(
            QString::fromLatin1(notificationIconName), summary, message, actions,
            notificationHints(urgencyNormal, actions.isEmpty()), defaultTimeoutMs);
        if (const auto* error = std::get_if<QString>(&sent))
        {
            qWarning().noquote() << "Failed to send desktop notification" << *error;
            return false;
        }

        const auto notificationId = std::get<uint>(sent);
        m_trackedNotifications.insert_or_assign(notificationId, TrackedNotification{
                                                                    .accountId = accountId,
                                                                    .mailboxId = mailboxId,
                                                                    .mailboxName = mailboxName,
                                                                    .threadId = threadId,
                                                                    .emailId = emailId,
                                                                    .activationToken = {},
                                                                    .connectionId = {},
                                                                    .calendarNotificationKey = {},
                                                                    .sendId = {},
                                                                    .opensSettings = false,
                                                                });
        return true;
    }

    void DesktopNotificationController::notifyError(const QString& connectionId,
                                                    const QString& title, const QString& message,
                                                    const bool persistent, const bool opensSettings)
    {
        const QStringList actions =
            opensSettings
                ? QStringList{QString::fromLatin1(defaultActionKey),
                              i18nc("@action:button desktop notification", "Open Settings")}
                : QStringList{};
        const auto sent =
            m_transport->send(QStringLiteral("dialog-warning"), title, message, actions,
                              notificationHints(persistent ? urgencyCritical : urgencyNormal),
                              persistent ? 0 : defaultTimeoutMs);
        if (const auto* error = std::get_if<QString>(&sent))
        {
            qWarning().noquote() << "Failed to send error notification" << *error;
            return;
        }

        m_trackedNotifications.insert_or_assign(
            std::get<uint>(sent), TrackedNotification{.accountId = {},
                                                      .mailboxId = {},
                                                      .mailboxName = {},
                                                      .threadId = {},
                                                      .emailId = {},
                                                      .activationToken = {},
                                                      .connectionId = connectionId,
                                                      .calendarNotificationKey = {},
                                                      .sendId = {},
                                                      .opensSettings = opensSettings});
    }

    bool DesktopNotificationController::notifyCalendarEvent(const QString& key,
                                                            const QString& title,
                                                            const QString& message)
    {
        const QStringList actions = {
            QStringLiteral("dismiss"), i18nc("@action:button desktop notification", "Dismiss"),
            QStringLiteral("snooze"), i18nc("@action:button desktop notification", "Snooze 5 min")};
        const auto sent = m_transport->send(QStringLiteral("x-office-calendar"), title, message,
                                            actions, notificationHints(urgencyNormal, false), 0);
        if (const auto* error = std::get_if<QString>(&sent))
        {
            qWarning().noquote() << "Failed to send calendar notification" << *error;
            return false;
        }
        m_trackedNotifications.insert_or_assign(std::get<uint>(sent),
                                                TrackedNotification{.accountId = {},
                                                                    .mailboxId = {},
                                                                    .mailboxName = {},
                                                                    .threadId = {},
                                                                    .emailId = {},
                                                                    .activationToken = {},
                                                                    .connectionId = {},
                                                                    .calendarNotificationKey = key,
                                                                    .sendId = {},
                                                                    .opensSettings = false});
        return true;
    }

    bool DesktopNotificationController::notifyCalendarInvitation(
        const QString& key, const QString& calendarAccountId, const QString& eventId,
        const QString& recurrenceId, const QString& navigationDate, const QString& title,
        const QString& message)
    {
        closeCalendarInvitation(key);
        const QStringList actions = {
            QString::fromLatin1(defaultActionKey),
            i18nc("@action:button desktop notification", "Open"),
        };
        const auto sent = m_transport->send(
            QStringLiteral("x-office-calendar"), i18n("Calendar invitation: %1", title), message,
            actions, notificationHints(urgencyNormal, true, false), 0);
        if (const auto* error = std::get_if<QString>(&sent))
        {
            qWarning().noquote() << "Failed to send calendar invitation notification" << *error;
            return false;
        }
        const auto notificationId = std::get<uint>(sent);
        m_invitationNotificationIds.insert(key, notificationId);
        m_trackedNotifications.insert_or_assign(
            notificationId, TrackedNotification{.accountId = {},
                                                .mailboxId = {},
                                                .mailboxName = {},
                                                .threadId = {},
                                                .emailId = {},
                                                .activationToken = {},
                                                .connectionId = {},
                                                .calendarNotificationKey = {},
                                                .calendarInvitationKey = key,
                                                .calendarAccountId = calendarAccountId,
                                                .calendarEventId = eventId,
                                                .calendarRecurrenceId = recurrenceId,
                                                .calendarNavigationDate = navigationDate,
                                                .sendId = {},
                                                .opensSettings = false});
        return true;
    }

    void DesktopNotificationController::closeCalendarInvitation(const QString& key)
    {
        const auto found = m_invitationNotificationIds.find(key);
        if (found == m_invitationNotificationIds.end())
            return;
        const auto notificationId = found.value();
        untrackNotification(notificationId);
        m_transport->close(notificationId);
    }

    bool DesktopNotificationController::notifyUndoableSend(const QString& sendId,
                                                           const QString& title,
                                                           const QString& message,
                                                           const int timeoutMs)
    {
        closeUndoableSendNotification(sendId);
        if (!m_actionInvokedConnected || !m_notificationClosedConnected)
            return false;

        // Undo Send is only safe while the notification service currently supports actions.
        // Refresh this infrequent capability check instead of relying on the new-mail burst cache.
        m_transportSupportsActions = m_transport->supportsActions();
        if (!*m_transportSupportsActions)
            return false;

        const QStringList actions = {undoActionKey(sendId),
                                     i18nc("@action:button desktop notification", "Undo Send")};
        // Plasma retains non-transient notifications in history after the popup disappears, so
        // NotificationClosed is not emitted when their display timeout ends.
        const auto sent =
            m_transport->send(QStringLiteral("mail-send"), title, message, actions,
                              notificationHints(urgencyNormal, false, true), timeoutMs);
        if (std::holds_alternative<QString>(sent))
            return false;
        const auto notificationId = std::get<uint>(sent);
        m_sendNotificationIds.insert(sendId, notificationId);
        m_trackedNotifications.insert_or_assign(notificationId,
                                                TrackedNotification{.accountId = {},
                                                                    .mailboxId = {},
                                                                    .mailboxName = {},
                                                                    .threadId = {},
                                                                    .emailId = {},
                                                                    .activationToken = {},
                                                                    .connectionId = {},
                                                                    .calendarNotificationKey = {},
                                                                    .sendId = sendId,
                                                                    .opensSettings = false});
        return true;
    }

    void DesktopNotificationController::closeUndoableSendNotification(const QString& sendId)
    {
        const auto found = m_sendNotificationIds.find(sendId);
        if (found == m_sendNotificationIds.end())
            return;
        const auto notificationId = found.value();
        untrackNotification(notificationId);
        m_transport->close(notificationId);
    }

    void DesktopNotificationController::closeAllUndoableSendNotifications()
    {
        std::vector<uint> notificationIds;
        notificationIds.reserve(static_cast<std::size_t>(m_sendNotificationIds.size()));
        for (const auto& [notificationId, tracked] : m_trackedNotifications)
        {
            if (!tracked.sendId.isEmpty())
                notificationIds.push_back(notificationId);
        }

        for (const auto notificationId : notificationIds)
        {
            untrackNotification(notificationId);
            m_transport->close(notificationId);
        }
    }

    void DesktopNotificationController::onActionInvoked(const uint notificationId,
                                                        const QString& actionKey)
    {
        const auto it = m_trackedNotifications.find(notificationId);
        if (it == m_trackedNotifications.end())
        {
            if (const auto sendId = sendIdFromUndoAction(actionKey))
                Q_EMIT undoSendRequested(*sendId);
            return;
        }

        const auto tracked = it->second;
        if (!tracked.calendarNotificationKey.isEmpty())
        {
            if (actionKey == QStringLiteral("dismiss") || actionKey == QStringLiteral("snooze"))
            {
                untrackNotification(notificationId);
                Q_EMIT calendarNotificationAction(tracked.calendarNotificationKey,
                                                  actionKey == QStringLiteral("snooze"));
            }
            return;
        }
        if (!tracked.calendarInvitationKey.isEmpty())
        {
            if (actionKey != QString::fromLatin1(defaultActionKey))
                return;
            Q_EMIT calendarInvitationActivated(
                tracked.calendarInvitationKey, tracked.calendarAccountId, tracked.calendarEventId,
                tracked.calendarRecurrenceId, tracked.calendarNavigationDate,
                tracked.activationToken);
            return;
        }
        if (!tracked.sendId.isEmpty())
        {
            const auto embeddedSendId = sendIdFromUndoAction(actionKey);
            if (!embeddedSendId || *embeddedSendId != tracked.sendId)
                return;
            untrackNotification(notificationId);
            Q_EMIT undoSendRequested(tracked.sendId);
            return;
        }
        if (tracked.opensSettings)
        {
            if (actionKey == QString::fromLatin1(defaultActionKey))
                Q_EMIT errorNotificationActivated(tracked.connectionId, tracked.activationToken);
            return;
        }

        if (actionKey == QString::fromLatin1(archiveActionKey))
        {
            untrackNotification(notificationId);
            m_transport->close(notificationId);
            Q_EMIT mailArchiveRequested(tracked.accountId, tracked.mailboxId, tracked.emailId);
            return;
        }
        if (actionKey == QString::fromLatin1(markReadActionKey))
        {
            untrackNotification(notificationId);
            m_transport->close(notificationId);
            Q_EMIT mailMarkReadRequested(tracked.accountId, tracked.emailId);
            return;
        }
        if (actionKey == QString::fromLatin1(replyActionKey))
        {
            untrackNotification(notificationId);
            m_transport->close(notificationId);
            Q_EMIT mailReplyRequested(tracked.accountId, tracked.emailId, tracked.activationToken);
            return;
        }
        if (actionKey != QString::fromLatin1(defaultActionKey))
            return;

        Q_EMIT notificationActivated(tracked.accountId, tracked.mailboxId, tracked.mailboxName,
                                     tracked.threadId, tracked.emailId, tracked.activationToken);
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
        if (found == m_trackedNotifications.end())
            return;

        const auto tracked = found->second;
        untrackNotification(notificationId);
        if (!tracked.calendarNotificationKey.isEmpty() && reason == dismissedByUserReason)
            Q_EMIT calendarNotificationAction(tracked.calendarNotificationKey, false);
        if (!tracked.sendId.isEmpty() && reason != 3)
            Q_EMIT undoableSendWindowEnded(tracked.sendId, closeReason(reason));
    }

    void
    DesktopNotificationController::onNotificationServiceUnregistered(const QString& serviceName)
    {
        if (serviceName != QString::fromLatin1(notificationsService))
            return;

        QSet<QString> sendIds;
        std::vector<uint> notificationIds;
        notificationIds.reserve(m_trackedNotifications.size());
        for (const auto& [notificationId, tracked] : m_trackedNotifications)
        {
            notificationIds.push_back(notificationId);
            if (!tracked.sendId.isEmpty())
                sendIds.insert(tracked.sendId);
        }
        for (const auto notificationId : notificationIds)
            untrackNotification(notificationId);
        m_sendNotificationIds.clear();
        m_invitationNotificationIds.clear();
        m_transportSupportsActions.reset();
        for (const auto& sendId : sendIds)
            Q_EMIT undoableSendWindowEnded(sendId,
                                           DesktopNotificationCloseReason::NotificationServiceLost);
    }

    bool DesktopNotificationController::transportSupportsActions()
    {
        if (!m_transportSupportsActions.has_value())
            m_transportSupportsActions = m_transport->supportsActions();
        return *m_transportSupportsActions;
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

    QVariantMap DesktopNotificationController::notificationHints(
        const int urgency, const bool associateWithDesktopEntry, const bool transient) const
    {
        QVariantMap hints;
        if (associateWithDesktopEntry)
            hints.insert(QStringLiteral("desktop-entry"), QString::fromLatin1(desktopEntryName));
        hints.insert(QStringLiteral("urgency"), urgency);
        if (transient)
            hints.insert(QStringLiteral("transient"), true);
        hints.insert(QStringLiteral("sender-pid"),
                     static_cast<qlonglong>(QCoreApplication::applicationPid()));
        return hints;
    }

    void DesktopNotificationController::untrackNotification(const uint notificationId)
    {
        const auto found = m_trackedNotifications.find(notificationId);
        if (found != m_trackedNotifications.end())
        {
            if (!found->second.sendId.isEmpty())
                m_sendNotificationIds.remove(found->second.sendId);
            if (!found->second.calendarInvitationKey.isEmpty())
                m_invitationNotificationIds.remove(found->second.calendarInvitationKey);
        }
        m_trackedNotifications.erase(notificationId);
    }

} // namespace javelin::app
