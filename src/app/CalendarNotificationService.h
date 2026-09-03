#pragma once

#include "storage/sqlite/DatabaseConnection.h"

#include "app/calendar/CalendarReminderMaterializationPort.h"
#include "jmap/cache/CalendarNotificationRepository.h"

#include <QCoroTask>

#include <QObject>

#include <unordered_map>
#include <unordered_set>

class QTimer;

namespace javelin::app::undo
{
    class CalendarHistoryPort;
}

namespace javelin::app
{
    class CalendarNotificationService final : public QObject
    {
        Q_OBJECT

      public:
        explicit CalendarNotificationService(
            javelin::jmap::cache::DatabaseConnection& connection,
            javelin::app::undo::CalendarHistoryPort& calendarEvents,
            CalendarReminderMaterializationPort& reminderMaterializer, QObject* parent = nullptr);
        void start();
        void requestScan();
        void deliveryAccepted(const QString& key);
        void deliveryFailed(const QString& key);
        void dismiss(const QString& key);
        void snooze(const QString& key);
        void calendarAlertReceived(const QString& ownerAccountId, const QString& accountId,
                                   const QString& eventId, const QString& uid,
                                   const QString& recurrenceId, const QString& alertId);
        void calendarMetadataReady(const QString& ownerAccountId);
        void calendarStateChanged(const QString& ownerAccountId);
        void calendarCacheCommitted(const QString& ownerAccountId);
        void calendarAccountRemoved(const QString& ownerAccountId);

      Q_SIGNALS:
        void reminderDue(const QString& key, const QString& title, const QString& message);
        void calendarMetadataRequired(const QString& ownerAccountId);

      private Q_SLOTS:
        void scan();
        void retryPushedAlerts();
        void retryPushedDeliveries();
        void refreshReminderHorizons();

      private:
        void
        processQueuedPushedAlerts(std::optional<std::string_view> ownerAccountId = std::nullopt);
        void schedulePushedAlertRetry(const QDateTime& retryAt);
        void requestReminderHorizon(std::string ownerAccountId);

        javelin::jmap::cache::DatabaseConnection& m_connection;
        javelin::jmap::cache::CalendarNotificationRepository m_repository;
        javelin::app::undo::CalendarHistoryPort& m_calendarEvents;
        CalendarReminderMaterializationPort& m_reminderMaterializer;
        QTimer* m_timer = nullptr;
        QTimer* m_pushRetryTimer = nullptr;
        QTimer* m_pushDeliveryRetryTimer = nullptr;
        QTimer* m_horizonRefreshTimer = nullptr;
        [[nodiscard]] QCoro::Task<void>
        resolvePushedAlert(javelin::jmap::cache::CalendarPushedAlert alert);
        [[nodiscard]] QCoro::Task<void>
        synchronizeDismissal(javelin::jmap::cache::CalendarNotificationCandidate candidate);

        std::unordered_map<std::string, javelin::jmap::cache::CalendarNotificationCandidate>
            m_candidates;
        std::unordered_set<std::string> m_pendingPushAlerts;
        std::unordered_map<std::string, javelin::jmap::cache::CalendarNotificationCandidate>
            m_retryPushDeliveries;
        std::unordered_set<std::string> m_horizonOwners;
        std::unordered_set<std::string> m_horizonRefreshesInFlight;
        std::unordered_set<std::string> m_horizonRefreshPending;
    };
} // namespace javelin::app
