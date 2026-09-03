#include "app/CalendarNotificationService.h"

#include "app/undo/CalendarHistoryPort.h"
#include "app/undo/HistoryTypes.h"
#include "jmap/OperationError.h"
#include "jmap/cache/CalendarRepository.h"
#include "jmap/calendar/CalendarEventEditing.h"

#include <QCoroTask>

#include <KLocalizedString>

#include <QDateTime>
#include <QDebug>
#include <QLocale>
#include <QTimeZone>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <vector>

namespace javelin::app
{
    namespace
    {
        constexpr auto pushedAlertRetryDelay = std::chrono::seconds{30};
        constexpr auto pushedDeliveryRetryDelay = std::chrono::seconds{60};
        constexpr auto maximumPushedAlertRetryDelay = std::chrono::minutes{5};
        constexpr auto reminderHorizonRefreshInterval = std::chrono::hours{6};
        constexpr auto reminderHorizonRetryDelay = std::chrono::minutes{1};
        constexpr int reminderHorizonPastDays = 1;
        constexpr int reminderHorizonFutureDays = 90;

        struct ReminderHorizonRequest
        {
            javelin::jmap::calendar::VisibleInterval interval;
            javelin::jmap::calendar::TimeZoneId displayTimeZone;
        };

        ReminderHorizonRequest reminderHorizonRequest(const QDateTime& now)
        {
            auto zone = QTimeZone::systemTimeZone();
            if (!zone.isValid())
                zone = QTimeZone::UTC;
            const auto localDate = now.toTimeZone(zone).date();
            const auto start =
                QDateTime{localDate.addDays(-reminderHorizonPastDays), QTime{0, 0}, zone};
            const auto end =
                QDateTime{localDate.addDays(reminderHorizonFutureDays + 1), QTime{0, 0}, zone};
            const auto localValue = [](const QDateTime& value)
            { return value.toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss")).toStdString(); };
            return {
                .interval = {.start = {.value = localValue(start)},
                             .end = {.value = localValue(end)}},
                .displayTimeZone = {.value = zone.id().toStdString()},
            };
        }

        int pushedAlertRetryDelayMs(const javelin::jmap::OperationError& error)
        {
            const auto requested = error.retryAfter.value_or(pushedAlertRetryDelay);
            const auto bounded = std::clamp(
                requested, std::chrono::seconds{1},
                std::chrono::duration_cast<std::chrono::seconds>(maximumPushedAlertRetryDelay));
            return static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(bounded).count());
        }

        QDateTime parseInstant(const std::string& value)
        {
            auto parsed = QDateTime::fromString(QString::fromStdString(value), Qt::ISODateWithMs);
            if (!parsed.isValid())
                parsed = QDateTime::fromString(QString::fromStdString(value), Qt::ISODate);
            return parsed.toUTC();
        }

        QTimeZone eventTimeZone(const javelin::jmap::calendar::CalendarEvent& event)
        {
            return event.timeZone ? QTimeZone{QByteArray::fromStdString(event.timeZone->value)}
                                  : QTimeZone::systemTimeZone();
        }

        QDateTime startsAtUtc(const javelin::jmap::calendar::CalendarEvent& event)
        {
            if (event.utcStart)
            {
                const auto parsed = parseInstant(event.utcStart->value);
                if (parsed.isValid())
                    return parsed;
            }

            const auto local =
                QDateTime::fromString(QString::fromStdString(event.start.value), Qt::ISODate);
            if (!local.isValid())
                return {};
            const auto zone = eventTimeZone(event);
            if (!zone.isValid())
                return {};
            return QDateTime{local.date(), local.time(), zone}.toUTC();
        }

        QDateTime endsAtUtc(const javelin::jmap::calendar::CalendarEvent& event,
                            const QDateTime& startsAt)
        {
            if (event.utcEnd)
            {
                const auto parsed = parseInstant(event.utcEnd->value);
                if (parsed.isValid())
                    return parsed;
            }
            const auto duration = javelin::jmap::calendar::durationSeconds(event.duration);
            return duration ? startsAt.addSecs(*duration) : QDateTime{};
        }

        std::string pushAlertPendingKey(const std::string_view ownerAccountId,
                                        const std::string_view accountId,
                                        const std::string_view eventId,
                                        const std::optional<std::string>& recurrenceId,
                                        const std::string_view alertId)
        {
            const auto append = [](std::string& key, const std::string_view value)
            {
                key += std::to_string(value.size());
                key.push_back(':');
                key.append(value);
            };
            std::string key{"push:"};
            append(key, ownerAccountId);
            append(key, accountId);
            append(key, eventId);
            append(key, recurrenceId ? std::string_view{*recurrenceId} : std::string_view{});
            append(key, alertId);
            return key;
        }
    } // namespace

    CalendarNotificationService::CalendarNotificationService(
        javelin::jmap::cache::DatabaseConnection& connection,
        javelin::app::undo::CalendarHistoryPort& calendarEvents,
        CalendarReminderMaterializationPort& reminderMaterializer, QObject* parent)
        : QObject(parent), m_connection(connection), m_repository(connection),
          m_calendarEvents(calendarEvents), m_reminderMaterializer(reminderMaterializer),
          m_timer(new QTimer(this)), m_pushRetryTimer(new QTimer(this)),
          m_pushDeliveryRetryTimer(new QTimer(this)), m_horizonRefreshTimer(new QTimer(this))
    {
        m_timer->setSingleShot(true);
        connect(m_timer, &QTimer::timeout, this, &CalendarNotificationService::scan);
        m_pushRetryTimer->setSingleShot(true);
        connect(m_pushRetryTimer, &QTimer::timeout, this,
                &CalendarNotificationService::retryPushedAlerts);
        m_pushDeliveryRetryTimer->setSingleShot(true);
        connect(m_pushDeliveryRetryTimer, &QTimer::timeout, this,
                &CalendarNotificationService::retryPushedDeliveries);
        m_horizonRefreshTimer->setInterval(static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(reminderHorizonRefreshInterval)
                .count()));
        connect(m_horizonRefreshTimer, &QTimer::timeout, this,
                &CalendarNotificationService::refreshReminderHorizons);
    }

    void CalendarNotificationService::start()
    {
        if (const auto error = m_repository.recoverDispatches())
            qWarning().noquote() << "Recover calendar notification delivery:" << error->message;
        javelin::jmap::cache::CalendarRepository calendars{m_connection};
        const auto accounts = calendars.listAccounts();
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&accounts))
            qWarning().noquote() << "Discover calendar reminder owners:" << error->message;
        else
            for (const auto& account :
                 std::get<std::vector<javelin::jmap::cache::CalendarAccount>>(accounts))
                m_horizonOwners.insert(account.ownerAccountId);
        retryPushedAlerts();
        refreshReminderHorizons();
        m_horizonRefreshTimer->start();
        scan();
    }

    void CalendarNotificationService::requestScan()
    {
        m_timer->start(0);
    }

    void CalendarNotificationService::calendarAlertReceived(
        const QString& ownerAccountId, const QString& accountId, const QString& eventId,
        const QString& uid, const QString& recurrenceId, const QString& alertId)
    {
        const auto recurrence = recurrenceId.isEmpty() ? std::optional<std::string>{}
                                                       : std::optional{recurrenceId.toStdString()};
        javelin::jmap::cache::CalendarPushedAlert alert{
            .key = pushAlertPendingKey(ownerAccountId.toStdString(), accountId.toStdString(),
                                       eventId.toStdString(), recurrence, alertId.toStdString()),
            .ownerAccountId = ownerAccountId.toStdString(),
            .accountId = accountId.toStdString(),
            .eventId = eventId.toStdString(),
            .uid = uid.toStdString(),
            .recurrenceId = recurrence,
            .alertId = alertId.toStdString(),
        };
        const auto queued = m_repository.enqueuePushed(alert);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&queued))
        {
            qWarning().noquote() << "Queue pushed calendar reminder:" << error->message;
            return;
        }
        if (!m_pendingPushAlerts.insert(alert.key).second)
            return;
        auto task = resolvePushedAlert(std::move(alert));
        QCoro::connect(std::move(task), this, []() {});
    }

    void CalendarNotificationService::calendarMetadataReady(const QString& ownerAccountId)
    {
        const auto owner = ownerAccountId.toStdString();
        processQueuedPushedAlerts(owner);
        requestReminderHorizon(owner);
    }

    void CalendarNotificationService::calendarStateChanged(const QString& ownerAccountId)
    {
        requestReminderHorizon(ownerAccountId.toStdString());
    }

    void CalendarNotificationService::calendarCacheCommitted(const QString& ownerAccountId)
    {
        requestScan();
        requestReminderHorizon(ownerAccountId.toStdString());
    }

    void CalendarNotificationService::calendarAccountRemoved(const QString& ownerAccountId)
    {
        const auto owner = ownerAccountId.toStdString();
        m_horizonOwners.erase(owner);
        m_horizonRefreshPending.erase(owner);
    }

    void CalendarNotificationService::refreshReminderHorizons()
    {
        const std::vector<std::string> owners{m_horizonOwners.begin(), m_horizonOwners.end()};
        for (const auto& owner : owners)
            requestReminderHorizon(owner);
    }

    void CalendarNotificationService::requestReminderHorizon(std::string ownerAccountId)
    {
        if (ownerAccountId.empty())
            return;
        m_horizonOwners.insert(ownerAccountId);
        if (!m_horizonRefreshesInFlight.insert(ownerAccountId).second)
        {
            m_horizonRefreshPending.insert(std::move(ownerAccountId));
            return;
        }

        const auto owner = ownerAccountId;
        auto request = reminderHorizonRequest(QDateTime::currentDateTimeUtc());
        auto task = m_reminderMaterializer.materializeCalendarReminderHorizon(
            ownerAccountId, std::move(request.interval), std::move(request.displayTimeZone));
        QCoro::connect(
            std::move(task), this,
            [this, owner](const javelin::jmap::calendar::CalendarRefreshResult& result)
            {
                m_horizonRefreshesInFlight.erase(owner);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    qWarning().noquote() << "Calendar reminder horizon refresh failed"
                                         << QString::fromStdString(owner) << error->message;
                    const auto delay =
                        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             reminderHorizonRetryDelay)
                                             .count());
                    QTimer::singleShot(delay, this,
                                       [this, owner]
                                       {
                                           if (m_horizonOwners.contains(owner))
                                               requestReminderHorizon(owner);
                                       });
                }
                else
                {
                    requestScan();
                }

                if (m_horizonRefreshPending.erase(owner) > 0 && m_horizonOwners.contains(owner))
                    requestReminderHorizon(owner);
            });
    }

    void CalendarNotificationService::retryPushedAlerts()
    {
        processQueuedPushedAlerts();
    }

    void CalendarNotificationService::processQueuedPushedAlerts(
        const std::optional<std::string_view> ownerAccountId)
    {
        const auto pending = m_repository.pendingPushed(ownerAccountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&pending))
        {
            qWarning().noquote() << "Read queued calendar reminders:" << error->message;
            schedulePushedAlertRetry(QDateTime::currentDateTimeUtc().addSecs(30));
            return;
        }
        for (auto alert : std::get<std::vector<javelin::jmap::cache::CalendarPushedAlert>>(pending))
        {
            if (!m_pendingPushAlerts.insert(alert.key).second)
                continue;
            auto task = resolvePushedAlert(std::move(alert));
            QCoro::connect(std::move(task), this, []() {});
        }
    }

    void CalendarNotificationService::schedulePushedAlertRetry(const QDateTime& retryAt)
    {
        const auto now = QDateTime::currentDateTimeUtc();
        const qint64 maximumDelay = static_cast<qint64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(maximumPushedAlertRetryDelay)
                .count());
        const qint64 delay = std::clamp(now.msecsTo(retryAt), qint64{100}, maximumDelay);
        if (!m_pushRetryTimer->isActive() || m_pushRetryTimer->remainingTime() > delay)
            m_pushRetryTimer->start(static_cast<int>(delay));
    }

    void CalendarNotificationService::retryPushedDeliveries()
    {
        auto retries = std::move(m_retryPushDeliveries);
        m_retryPushDeliveries.clear();
        bool retryAgain = false;
        for (auto& [key, candidate] : retries)
        {
            const auto claimed =
                m_repository.claimPushed(candidate.identityKey, QDateTime::currentDateTimeUtc());
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&claimed))
            {
                qWarning().noquote()
                    << "Reclaim pushed calendar reminder delivery:" << error->message;
                m_retryPushDeliveries.insert_or_assign(key, std::move(candidate));
                retryAgain = true;
                continue;
            }
            const auto claim = std::get<javelin::jmap::cache::CalendarPushedAlertClaim>(claimed);
            if (!claim.claimed)
            {
                if (claim.completed && candidate.pushedAlert)
                {
                    if (const auto error = m_repository.removePushed(candidate.pushedAlert->key))
                        qWarning().noquote()
                            << "Complete queued calendar reminder:" << error->message;
                    continue;
                }
                m_retryPushDeliveries.insert_or_assign(key, std::move(candidate));
                retryAgain = true;
                continue;
            }
            m_candidates.insert_or_assign(key, candidate);
            const QString message =
                candidate.startsAt.isValid()
                    ? i18n("Starts %1", QLocale().toString(candidate.startsAt.toLocalTime(),
                                                           QLocale::ShortFormat))
                    : i18n("Calendar reminder");
            Q_EMIT reminderDue(QString::fromStdString(key), QString::fromStdString(candidate.title),
                               message);
        }
        if (retryAgain && !m_pushDeliveryRetryTimer->isActive())
            m_pushDeliveryRetryTimer->start(static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(pushedDeliveryRetryDelay)
                    .count()));
    }

    QCoro::Task<void>
    CalendarNotificationService::resolvePushedAlert(javelin::jmap::cache::CalendarPushedAlert alert)
    {
        const auto completePending = [this, &alert] { m_pendingPushAlerts.erase(alert.key); };
        const auto discard = [this, &alert, &completePending]
        {
            if (const auto error = m_repository.removePushed(alert.key))
                qWarning().noquote() << "Remove queued calendar reminder:" << error->message;
            completePending();
        };

        auto result = co_await m_calendarEvents.getAuthoritativeCalendarEvent(
            alert.ownerAccountId, alert.accountId, alert.eventId, alert.uid);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            if (javelin::jmap::isTransientError(*error))
            {
                completePending();
                schedulePushedAlertRetry(
                    QDateTime::currentDateTimeUtc().addMSecs(pushedAlertRetryDelayMs(*error)));
                qWarning().noquote()
                    << "Fetch pushed calendar reminder; retrying:" << error->message;
                co_return;
            }
            qWarning().noquote() << "Fetch pushed calendar reminder:" << error->message;
            discard();
            co_return;
        }
        auto& authoritative = std::get<javelin::jmap::calendar::AuthoritativeCalendarEvent>(result);
        if (!authoritative.event || authoritative.event->uid != alert.uid ||
            authoritative.event->isDraft)
        {
            discard();
            co_return;
        }

        const auto eligibility =
            m_repository.notificationEligibility(alert.accountId, *authoritative.event);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&eligibility))
        {
            qWarning().noquote() << "Resolve pushed calendar reminder eligibility:"
                                 << error->message;
            completePending();
            schedulePushedAlertRetry(QDateTime::currentDateTimeUtc().addSecs(30));
            co_return;
        }
        const auto eligibilityValue =
            std::get<javelin::jmap::cache::CalendarNotificationEligibility>(eligibility);
        if (eligibilityValue ==
            javelin::jmap::cache::CalendarNotificationEligibility::MetadataMissing)
        {
            completePending();
            Q_EMIT calendarMetadataRequired(QString::fromStdString(alert.ownerAccountId));
            schedulePushedAlertRetry(QDateTime::currentDateTimeUtc().addSecs(30));
            co_return;
        }
        if (eligibilityValue == javelin::jmap::cache::CalendarNotificationEligibility::NotEligible)
        {
            discard();
            co_return;
        }

        auto effective = *authoritative.event;
        if (alert.recurrenceId)
        {
            const auto occurrence = javelin::jmap::calendar::effectiveOccurrenceEvent(
                *authoritative.event, {.value = *alert.recurrenceId});
            if (!occurrence)
            {
                discard();
                co_return;
            }
            effective = *occurrence;
        }

        const auto alertResult =
            m_repository.effectiveAlert(alert.accountId, *authoritative.event, alert.alertId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&alertResult))
        {
            qWarning().noquote() << "Resolve pushed calendar reminder:" << error->message;
            completePending();
            schedulePushedAlertRetry(QDateTime::currentDateTimeUtc().addSecs(30));
            co_return;
        }
        const auto& resolvedAlert =
            std::get<std::optional<javelin::jmap::calendar::Alert>>(alertResult);
        if (!resolvedAlert || resolvedAlert->action != "display")
        {
            discard();
            co_return;
        }

        const auto startsAt = startsAtUtc(effective);
        const auto endsAt = endsAtUtc(effective, startsAt);
        const auto trigger = javelin::jmap::calendar::alertTrigger(*resolvedAlert, startsAt, endsAt,
                                                                   eventTimeZone(effective));
        if (!startsAt.isValid() || !trigger.isValid() ||
            (resolvedAlert->acknowledged &&
             parseInstant(resolvedAlert->acknowledged->value) >= trigger))
        {
            discard();
            co_return;
        }
        const auto identityRecurrence =
            resolvedAlert->triggerKind == javelin::jmap::calendar::AlertTriggerKind::Absolute
                ? std::optional<std::string>{}
                : alert.recurrenceId;
        const auto notificationKey = javelin::jmap::cache::calendarNotificationIdentityKey(
            alert.accountId, alert.eventId, identityRecurrence, alert.alertId, trigger);
        if (m_candidates.contains(notificationKey))
        {
            completePending();
            schedulePushedAlertRetry(QDateTime::currentDateTimeUtc().addSecs(60));
            co_return;
        }
        const auto claimed =
            m_repository.claimPushed(notificationKey, QDateTime::currentDateTimeUtc());
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&claimed))
        {
            qWarning().noquote() << "Claim pushed calendar reminder:" << error->message;
            completePending();
            schedulePushedAlertRetry(QDateTime::currentDateTimeUtc().addSecs(30));
            co_return;
        }
        const auto claim = std::get<javelin::jmap::cache::CalendarPushedAlertClaim>(claimed);
        if (!claim.claimed)
        {
            if (claim.completed)
            {
                discard();
                co_return;
            }
            completePending();
            schedulePushedAlertRetry(
                claim.retryAt.value_or(QDateTime::currentDateTimeUtc().addSecs(30)));
            co_return;
        }

        completePending();
        auto candidate = javelin::jmap::cache::CalendarNotificationCandidate{
            .key = notificationKey,
            .identityKey = notificationKey,
            .ownerAccountId = alert.ownerAccountId,
            .accountId = alert.accountId,
            .eventId = alert.eventId,
            .occurrenceId = alert.recurrenceId.value_or(alert.eventId),
            .recurrenceId = alert.recurrenceId,
            .alertId = alert.alertId,
            .title = effective.title,
            .startsAt = startsAt,
            .alert = *resolvedAlert,
            .pushedAlert = alert,
        };
        m_candidates.insert_or_assign(notificationKey, candidate);
        const QString message = startsAt.isValid()
                                    ? i18n("Starts %1", QLocale().toString(startsAt.toLocalTime(),
                                                                           QLocale::ShortFormat))
                                    : i18n("Calendar reminder");
        Q_EMIT reminderDue(QString::fromStdString(notificationKey),
                           QString::fromStdString(candidate.title), message);
        co_return;
    }

    void CalendarNotificationService::deliveryAccepted(const QString& key)
    {
        const auto keyValue = key.toStdString();
        const auto found = m_candidates.find(keyValue);
        const std::string_view identityKey = found == m_candidates.end()
                                                 ? std::string_view{keyValue}
                                                 : std::string_view{found->second.identityKey};
        const std::optional<std::string_view> pushedAlertKey =
            found != m_candidates.end() && found->second.pushedAlert
                ? std::optional<std::string_view>{found->second.pushedAlert->key}
                : std::nullopt;
        if (const auto error = m_repository.markDelivered(
                keyValue, identityKey, QDateTime::currentDateTimeUtc(), pushedAlertKey))
            qWarning().noquote() << "Record calendar notification delivery:" << error->message;
    }

    void CalendarNotificationService::deliveryFailed(const QString& key)
    {
        const auto keyValue = key.toStdString();
        const auto found = m_candidates.find(keyValue);
        const std::string identityKey =
            found == m_candidates.end() ? keyValue : found->second.identityKey;
        if (const auto error = m_repository.releaseDispatch(identityKey))
            qWarning().noquote() << "Release calendar notification delivery:" << error->message;
        if (found != m_candidates.end() && found->second.key == found->second.identityKey)
        {
            m_retryPushDeliveries.insert_or_assign(keyValue, found->second);
            m_candidates.erase(found);
            if (!m_pushDeliveryRetryTimer->isActive())
                m_pushDeliveryRetryTimer->start(static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(pushedDeliveryRetryDelay)
                        .count()));
            return;
        }
        m_candidates.erase(keyValue);
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
        auto candidate = found->second;
        m_candidates.erase(found);
        auto task = synchronizeDismissal(std::move(candidate));
        QCoro::connect(std::move(task), this, []() {});
    }

    QCoro::Task<void> CalendarNotificationService::synchronizeDismissal(
        javelin::jmap::cache::CalendarNotificationCandidate candidate)
    {
        std::optional<javelin::jmap::calendar::CalendarEvent> event;
        javelin::jmap::cache::CalendarRepository repository{m_connection};
        const auto cached = repository.findEvent(candidate.accountId, candidate.eventId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&cached))
        {
            qWarning().noquote() << "Read calendar reminder event for dismissal:" << error->message;
            co_return;
        }
        event = std::get<std::optional<javelin::jmap::calendar::CalendarEvent>>(cached);

        if (!event && candidate.pushedAlert)
        {
            auto authoritative = co_await m_calendarEvents.getAuthoritativeCalendarEvent(
                candidate.ownerAccountId, candidate.accountId, candidate.eventId,
                candidate.pushedAlert->uid);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&authoritative))
            {
                qWarning().noquote()
                    << "Fetch calendar reminder event for dismissal:" << error->message;
                co_return;
            }
            const auto& result =
                std::get<javelin::jmap::calendar::AuthoritativeCalendarEvent>(authoritative);
            if (!result.event || result.event->uid != candidate.pushedAlert->uid)
                co_return;
            event = *result.event;
        }
        if (!event)
            co_return;

        auto result = co_await m_calendarEvents.updateCalendarEvent(
            candidate.ownerAccountId,
            {.accountId = candidate.accountId,
             .event = javelin::jmap::calendar::acknowledgeAlert(
                 std::move(*event), candidate.alert,
                 {.value =
                      QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString()}),
             .operationGroupId = std::nullopt,
             .ifInState = std::nullopt,
             .materialization = std::nullopt},
            javelin::app::undo::CommandOrigin::SystemChild);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
            qWarning().noquote() << "Synchronize calendar reminder dismissal:" << error->message;
        co_return;
    }

    void CalendarNotificationService::snooze(const QString& key)
    {
        const auto keyValue = key.toStdString();
        const auto found = m_candidates.find(keyValue);
        const auto until = QDateTime::currentDateTimeUtc().addSecs(300);
        const auto pushedAlert = found == m_candidates.end()
                                     ? std::optional<javelin::jmap::cache::CalendarPushedAlert>{}
                                     : found->second.pushedAlert;
        if (const auto error = m_repository.snooze(keyValue, until, pushedAlert))
        {
            qWarning().noquote() << error->message;
            return;
        }
        m_candidates.erase(keyValue);
        if (pushedAlert)
            schedulePushedAlertRetry(until);
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
