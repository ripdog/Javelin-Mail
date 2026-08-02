#pragma once

#include <QObject>

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVariantMap>

#include <memory>
#include <unordered_map>
#include <variant>

namespace javelin::app
{
    class DesktopNotificationTransport
    {
      public:
        virtual ~DesktopNotificationTransport() = default;

        [[nodiscard]] virtual std::variant<uint, QString>
        send(const QString& icon, const QString& summary, const QString& message,
             const QStringList& actions, const QVariantMap& hints, int timeoutMs) = 0;
        virtual void close(uint notificationId) = 0;
    };

    class DesktopNotificationController final : public QObject
    {
        Q_OBJECT

      public:
        explicit DesktopNotificationController(QObject* parent = nullptr);
        DesktopNotificationController(std::unique_ptr<DesktopNotificationTransport> transport,
                                      bool connectSignals, QObject* parent = nullptr);

        [[nodiscard]] bool notifyNewMail(const QString& accountId, const QString& mailboxId,
                                         const QString& threadId, const QString& emailId,
                                         const QString& mailboxName, const QString& title,
                                         const QString& message);
        void notifyError(const QString& connectionId, const QString& title, const QString& message,
                         bool persistent, bool opensSettings);
        [[nodiscard]] bool notifyCalendarEvent(const QString& key, const QString& title,
                                               const QString& message);
        void notifyUndoableSend(const QString& sendId, const QString& title, const QString& message,
                                int timeoutMs);
        void closeUndoableSendNotification(const QString& sendId);

      Q_SIGNALS:
        void notificationActivated(const QString& accountId, const QString& mailboxId,
                                   const QString& mailboxName, const QString& threadId,
                                   const QString& emailId, const QString& activationToken);
        void errorNotificationActivated(const QString& connectionId,
                                        const QString& activationToken);
        void calendarNotificationAction(const QString& key, bool snooze);
        void undoSendRequested(const QString& sendId);

      private Q_SLOTS:
        void onActionInvoked(uint notificationId, const QString& actionKey);
        void onActivationToken(uint notificationId, const QString& activationToken);
        void onNotificationClosed(uint notificationId, uint reason);

      private:
        struct TrackedNotification
        {
            QString accountId;
            QString mailboxId;
            QString mailboxName;
            QString threadId;
            QString emailId;
            QString activationToken;
            QString connectionId;
            QString calendarNotificationKey;
            QString sendId;
            bool opensSettings = false;
        };

        bool connectSignal(const char* signalName, const char* slotName);
        [[nodiscard]] QVariantMap notificationHints(int urgency,
                                                    bool activatesApplication = true) const;
        void untrackNotification(uint notificationId);

        std::unique_ptr<DesktopNotificationTransport> m_transport;
        std::unordered_map<uint, TrackedNotification> m_trackedNotifications;
        QHash<QString, uint> m_sendNotificationIds;
    };

} // namespace javelin::app
