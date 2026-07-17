#include "app/CalendarNotificationService.h"

#include <QDateTime>
#include <QDebug>
#include <QLocale>
#include <QTimer>

namespace javelin::app
{
    CalendarNotificationService::CalendarNotificationService(
        javelin::jmap::cache::DatabaseConnection& connection, QObject* parent)
        : QObject(parent), m_repository(connection), m_timer(new QTimer(this))
    {
        m_timer->setInterval(60000);
        connect(m_timer, &QTimer::timeout, this, &CalendarNotificationService::scan);
    }

    void CalendarNotificationService::start()
    {
        scan();
        m_timer->start();
    }

    void CalendarNotificationService::dismiss(const QString& key)
    {
        if (const auto error = m_repository.dismiss(key.toStdString()))
            qWarning().noquote() << error->message;
    }

    void CalendarNotificationService::snooze(const QString& key)
    {
        if (const auto error = m_repository.snooze(key.toStdString(),
                                                   QDateTime::currentDateTimeUtc().addSecs(300)))
            qWarning().noquote() << error->message;
    }

    void CalendarNotificationService::scan()
    {
        const auto result = m_repository.claimDue(QDateTime::currentDateTimeUtc());
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
        {
            qWarning().noquote() << error->message;
            return;
        }
        for (const auto& reminder :
             std::get<std::vector<javelin::jmap::cache::CalendarNotificationCandidate>>(result))
        {
            const QString localStart =
                QLocale().toString(reminder.startsAt.toLocalTime(), QLocale::ShortFormat);
            Q_EMIT reminderDue(QString::fromStdString(reminder.key),
                               QString::fromStdString(reminder.title),
                               QStringLiteral("Starts %1").arg(localStart));
        }
    }
} // namespace javelin::app
