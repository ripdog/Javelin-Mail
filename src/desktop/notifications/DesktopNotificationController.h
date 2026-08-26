#pragma once

#include <QObject>

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVariantMap>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <variant>

class QDBusServiceWatcher;

namespace javelin::app
{
    enum class DesktopNotificationCloseReason : uint
    {
        Expired = 1,
        DismissedByUser = 2,
        ClosedByApplication = 3,
        Undefined = 4,
        NotificationServiceLost,
    };

    class DesktopNotificationTransport
    {
      public:
        virtual ~DesktopNotificationTransport() = default;

        [[nodiscard]] virtual std::variant<uint, QString>
        send(const QString& icon, const QString& summary, const QString& message,
             const QStringList& actions, const QVariantMap& hints, int timeoutMs) = 0;
        [[nodiscard]] virtual bool supportsActions() const = 0;
        virtual void close(uint notificationId) = 0;
    };

    class DesktopNotificationController final : public QObject
    {
        Q_OBJECT

      public:
        explicit DesktopNotificationController(QObject* parent = nullptr);
        DesktopNotificationController(std::unique_ptr<DesktopNotificationTransport> transport,
                                      bool connectSignals, QObject* parent = nullptr);
        DesktopNotificationController(std::unique_ptr<DesktopNotificationTransport> transport,
                                      bool connectSignals, bool signalSubscriptionsConnected,
                                      QObject* parent = nullptr);
        ~DesktopNotificationController() override;

        [[nodiscard]] bool notifyNewMail(const QString& accountId, const QString& mailboxId,
                                         const QString& threadId, const QString& emailId,
                                         const QString& mailboxName, const QString& title,
                                         const QString& message);
        void notifyError(const QString& connectionId, const QString& title, const QString& message,
                         bool persistent, bool opensSettings);
        [[nodiscard]] bool notifyCalendarEvent(const QString& key, const QString& title,
                                               const QString& message);
        [[nodiscard]] bool notifyCalendarInvitation(const QString& key,
                                                    const QString& calendarAccountId,
                                                    const QString& eventId,
                                                    const QString& recurrenceId,
                                                    const QString& navigationDate,
                                                    const QString& title, const QString& message);
        void closeCalendarInvitation(const QString& key);
        [[nodiscard]] bool notifyUndoableSend(const QString& sendId, const QString& title,
                                              const QString& message, int timeoutMs);
        void closeUndoableSendNotification(const QString& sendId);
        void closeAllUndoableSendNotifications();

      Q_SIGNALS:
        void notificationActivated(const QString& accountId, const QString& mailboxId,
                                   const QString& mailboxName, const QString& threadId,
                                   const QString& emailId, const QString& activationToken);
        void mailArchiveRequested(const QString& accountId, const QString& mailboxId,
                                  const QString& emailId);
        void mailMarkReadRequested(const QString& accountId, const QString& emailId);
        void mailReplyRequested(const QString& accountId, const QString& emailId,
                                const QString& activationToken);
        void errorNotificationActivated(const QString& connectionId,
                                        const QString& activationToken);
        void calendarNotificationAction(const QString& key, bool snooze);
        void calendarInvitationActivated(const QString& key, const QString& calendarAccountId,
                                         const QString& eventId, const QString& recurrenceId,
                                         const QString& navigationDate,
                                         const QString& activationToken);
        void undoSendRequested(const QString& sendId);
        void undoableSendWindowEnded(const QString& sendId,
                                     javelin::app::DesktopNotificationCloseReason reason);

      private Q_SLOTS:
        void onActionInvoked(uint notificationId, const QString& actionKey);
        void onActivationToken(uint notificationId, const QString& activationToken);
        void onNotificationClosed(uint notificationId, uint reason);
        void onNotificationServiceUnregistered(const QString& serviceName);

      private:
        struct TrackedNotification
        {
            QString accountId = {};
            QString mailboxId = {};
            QString mailboxName = {};
            QString threadId = {};
            QString emailId = {};
            QString activationToken = {};
            QString connectionId = {};
            QString calendarNotificationKey = {};
            QString calendarInvitationKey = {};
            QString calendarAccountId = {};
            QString calendarEventId = {};
            QString calendarRecurrenceId = {};
            QString calendarNavigationDate = {};
            QString sendId = {};
            bool opensSettings = false;
        };

        bool connectSignal(const char* signalName, const char* slotName);
        [[nodiscard]] QVariantMap notificationHints(int urgency, bool activatesApplication = true,
                                                    bool transient = false) const;
        void untrackNotification(uint notificationId);

        std::unique_ptr<DesktopNotificationTransport> m_transport;
        std::unique_ptr<QDBusServiceWatcher> m_notificationServiceWatcher;
        std::unordered_map<uint, TrackedNotification> m_trackedNotifications;
        QHash<QString, uint> m_sendNotificationIds;
        QHash<QString, uint> m_invitationNotificationIds;
        bool m_actionInvokedConnected = false;
        bool m_notificationClosedConnected = false;
    };

} // namespace javelin::app
