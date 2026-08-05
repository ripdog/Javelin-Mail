#include "app/CalendarNotificationService.h"
#include "app/MailApplicationService.h"

#include "app/undo/HistoryTypes.h"
#include "jmap/cache/CalendarRepository.h"
#include "jmap/calendar/CalendarEventEditing.h"

#include <QCoroTask>

#include <KLocalizedString>

#include <QDateTime>
#include <QDebug>
#include <QLocale>
#include <QTimer>

#include <algorithm>

namespace javelin::app
{
    CalendarNotificationService::CalendarNotificationService(
        javelin::jmap::cache::DatabaseConnection& connection, MailApplicationService& mailService,
        QObject* parent)
        : QObject(parent), m_connection(connection), m_repository(connection),
          m_mailService(mailService), m_timer(new QTimer(this))
    {
        m_timer->setSingleShot(true);
        connect(m_timer, &QTimer::timeout, this, &CalendarNotificationService::scan);
    }

    void CalendarNotificationService::start()
    {
        if (const auto error = m_repository.recoverDispatches())
            qWarning().noquote() << "Recover calendar notification delivery:" << error->message;
        scan();
    }

    void CalendarNotificationService::requestScan()
    {
        m_timer->start(0);
    }

    void CalendarNotificationService::deliveryAccepted(const QString& key)
    {
        if (const auto error =
                m_repository.markDelivered(key.toStdString(), QDateTime::currentDateTimeUtc()))
            qWarning().noquote() << "Record calendar notification delivery:" << error->message;
    }

    void CalendarNotificationService::deliveryFailed(const QString& key)
    {
        if (const auto error = m_repository.releaseDispatch(key.toStdString()))
            qWarning().noquote() << "Release calendar notification delivery:" << error->message;
        m_candidates.erase(key.toStdString());
        m_timer->start(60000);
    }

    void CalendarNotificationService::dismiss(const QString& key)
    {
        if (const auto error = m_repository.dismiss(key.toStdString()))
        {
            qWarning().noquote() << error->message;
            return;
        }
        const auto found = m_candidates.find(key.toStdString());
        if (found == m_candidates.end() || found->second.ownerAccountId.empty())
            return;
        const auto candidate = found->second;
        m_candidates.erase(found);

        javelin::jmap::cache::CalendarRepository repository{m_connection};
        const auto cached = repository.findEvent(candidate.accountId, candidate.eventId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&cached))
        {
            qWarning().noquote() << error->message;
            return;
        }
        const auto& event = std::get<std::optional<javelin::jmap::calendar::CalendarEvent>>(cached);
        if (!event)
            return;
        const auto state = repository.stateToken(candidate.accountId, "CalendarEvent");
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&state))
        {
            qWarning().noquote() << error->message;
            return;
        }
        auto task = m_mailService.updateCalendarEvent(
            candidate.ownerAccountId,
            {.accountId = candidate.accountId,
             .event = javelin::jmap::calendar::acknowledgeAlert(
                 *event, candidate.alert,
                 {.value =
                      QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString()}),
             .operationGroupId = std::nullopt,
             .ifInState = std::get<std::optional<std::string>>(state),
             .materialization = std::nullopt},
            javelin::app::undo::CommandOrigin::SystemChild);
        QCoro::connect(std::move(task), this,
                       [](const javelin::jmap::calendar::CalendarMutationResult& result)
                       {
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&result))
                               qWarning().noquote()
                                   << "Synchronize calendar reminder dismissal:" << error->message;
                       });
    }

    void CalendarNotificationService::snooze(const QString& key)
    {
        if (const auto error = m_repository.snooze(key.toStdString(),
                                                   QDateTime::currentDateTimeUtc().addSecs(300)))
            qWarning().noquote() << error->message;
        m_candidates.erase(key.toStdString());
    }

    void CalendarNotificationService::scan()
    {
        const auto now = QDateTime::currentDateTimeUtc();
        const auto result = m_repository.claimDue(now);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
        {
            qWarning().noquote() << error->message;
            m_timer->start(60000);
            return;
        }
        for (const auto& reminder :
             std::get<std::vector<javelin::jmap::cache::CalendarNotificationCandidate>>(result))
        {
            m_candidates.insert_or_assign(reminder.key, reminder);
            const QString localStart =
                QLocale().toString(reminder.startsAt.toLocalTime(), QLocale::ShortFormat);
            Q_EMIT reminderDue(QString::fromStdString(reminder.key),
                               QString::fromStdString(reminder.title),
                               i18n("Starts %1", localStart));
        }
        constexpr qint64 maximumRescanMs = 5 * 60 * 1000;
        const auto nextTrigger = m_repository.nextTrigger();
        const auto delay = nextTrigger
                               ? std::clamp(now.msecsTo(*nextTrigger), 100LL, maximumRescanMs)
                               : maximumRescanMs;
        m_timer->start(static_cast<int>(delay));
    }
} // namespace javelin::app
