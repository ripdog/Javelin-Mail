#pragma once

#include "jmap/cache/CalendarNotificationRepository.h"

#include <QObject>

#include <unordered_map>

class QTimer;

namespace javelin::app
{
    class MailApplicationService;

    class CalendarNotificationService final : public QObject
    {
        Q_OBJECT

      public:
        explicit CalendarNotificationService(javelin::jmap::cache::DatabaseConnection& connection,
                                             MailApplicationService& mailService,
                                             QObject* parent = nullptr);
        void start();
        void requestScan();
        void dismiss(const QString& key);
        void snooze(const QString& key);

      Q_SIGNALS:
        void reminderDue(const QString& key, const QString& title, const QString& message);

      private Q_SLOTS:
        void scan();

      private:
        javelin::jmap::cache::DatabaseConnection& m_connection;
        javelin::jmap::cache::CalendarNotificationRepository m_repository;
        MailApplicationService& m_mailService;
        QTimer* m_timer = nullptr;
        std::unordered_map<std::string, javelin::jmap::cache::CalendarNotificationCandidate>
            m_candidates;
    };
} // namespace javelin::app
