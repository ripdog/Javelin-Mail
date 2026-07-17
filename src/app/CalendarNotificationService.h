#pragma once

#include "jmap/cache/CalendarNotificationRepository.h"

#include <QObject>

class QTimer;

namespace javelin::app
{
    class CalendarNotificationService final : public QObject
    {
        Q_OBJECT

      public:
        explicit CalendarNotificationService(javelin::jmap::cache::DatabaseConnection& connection,
                                             QObject* parent = nullptr);
        void start();
        void dismiss(const QString& key);
        void snooze(const QString& key);

      Q_SIGNALS:
        void reminderDue(const QString& key, const QString& title, const QString& message);

      private Q_SLOTS:
        void scan();

      private:
        javelin::jmap::cache::CalendarNotificationRepository m_repository;
        QTimer* m_timer = nullptr;
    };
} // namespace javelin::app
