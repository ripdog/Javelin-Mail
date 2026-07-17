#pragma once

#include <QObject>

#include <QByteArray>
#include <QString>
#include <QVariantMap>

#include <unordered_map>

namespace javelin::app
{

    class DesktopNotificationController final : public QObject
    {
        Q_OBJECT

      public:
        explicit DesktopNotificationController(QObject* parent = nullptr);

        void notifyNewMail(const QString& accountId, const QString& mailboxId,
                           const QString& threadId, const QString& emailId,
                           const QString& mailboxName, const QString& title,
                           const QString& message);
        void notifyError(const QString& connectionId, const QString& title, const QString& message,
                         bool persistent, bool opensSettings);
        void notifyCalendarEvent(const QString& key, const QString& title, const QString& message);

      Q_SIGNALS:
        void notificationActivated(const QString& accountId, const QString& mailboxId,
                                   const QString& threadId, const QString& emailId,
                                   const QString& activationToken);
        void errorNotificationActivated(const QString& connectionId,
                                        const QString& activationToken);
        void calendarNotificationAction(const QString& key, bool snooze);

      private Q_SLOTS:
        void onActionInvoked(uint notificationId, const QString& actionKey);
        void onActivationToken(uint notificationId, const QString& activationToken);
        void onNotificationClosed(uint notificationId, uint reason);

      private:
        struct TrackedNotification
        {
            QString accountId;
            QString mailboxId;
            QString threadId;
            QString emailId;
            QString activationToken;
            QString connectionId;
            QString calendarNotificationKey;
            bool opensSettings = false;
        };

        bool connectSignal(const char* signalName, const char* slotName);
        [[nodiscard]] QVariantMap notificationHints(int urgency) const;
        void untrackNotification(uint notificationId);

        std::unordered_map<uint, TrackedNotification> m_trackedNotifications;
    };

} // namespace javelin::app
