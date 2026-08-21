#include "gui/calendar/CalendarPresentation.h"
#include "gui/calendar/DayAgendaDialog.h"

#include <KLocalizedString>

#include <QDateTime>
#include <QLocale>

#include <array>
#include <cstdint>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

namespace javelin::gui::calendar
{
    namespace
    {
        constexpr std::array automaticCalendarHues{210, 24,  145, 282, 355, 45, 180, 320,
                                                   95,  250, 10,  165, 300, 70, 225, 340};

        void hashCalendarIdentityPart(std::uint64_t& hash, const std::string_view part)
        {
            constexpr std::uint64_t fnvPrime = 1099511628211ULL;
            for (const auto byte : part)
            {
                hash ^= static_cast<unsigned char>(byte);
                hash *= fnvPrime;
            }
            hash ^= 0xffU;
            hash *= fnvPrime;
        }
    } // namespace

    DayAgendaEvent dayAgendaEventFromMonthEvent(const MonthEvent& event)
    {
        return DayAgendaEvent{
            .key =
                {
                    .accountId = QString::fromStdString(event.accountId),
                    .eventId = QString::fromStdString(event.eventId),
                    .recurrenceId =
                        QString::fromStdString(event.recurrenceId.value_or(std::string{})),
                },
            .title = event.title,
            .calendarName = {},
            .color = event.color,
            .start = event.start,
            .end = event.end,
            .allDay = event.allDay,
            .recurring = event.recurring,
            .editable = false,
            .rsvpAllowed = false,
            .rsvpRecurrenceId = {},
            .participationStatus = {},
            .responseMutationPending = false,
            .responseError = {},
            .organizer = {},
            .location = {},
            .description = {},
            .attendees = {},
        };
    }

    std::optional<std::size_t> preferredNewEventCalendarIndex(
        const std::span<const NewEventCalendarCandidate> candidates,
        const javelin::protocol::CalendarDefaultDestination& configuredDestination)
    {
        const bool hasConfiguredDestination = !configuredDestination.ownerAccountId.isEmpty() &&
                                              !configuredDestination.accountId.isEmpty() &&
                                              !configuredDestination.calendarId.isEmpty();
        std::optional<std::size_t> serverDefaultIndex;
        std::optional<std::size_t> firstWritableIndex;
        for (std::size_t index = 0; index < candidates.size(); ++index)
        {
            const auto& candidate = candidates[index];
            if (!candidate.writable)
                continue;
            if (!firstWritableIndex.has_value())
                firstWritableIndex = index;
            if (!serverDefaultIndex.has_value() && candidate.serverDefault)
                serverDefaultIndex = index;
            if (hasConfiguredDestination &&
                configuredDestination.ownerAccountId ==
                    QString::fromStdString(candidate.ownerAccountId) &&
                configuredDestination.accountId == QString::fromStdString(candidate.accountId) &&
                configuredDestination.calendarId == QString::fromStdString(candidate.calendarId))
                return index;
        }
        return serverDefaultIndex.has_value() ? serverDefaultIndex : firstWritableIndex;
    }

    QString eventConfirmationDetails(const javelin::jmap::calendar::CalendarEvent& event)
    {
        const QString title =
            event.title.empty() ? i18n("Untitled event") : QString::fromStdString(event.title);
        const auto start =
            QDateTime::fromString(QString::fromStdString(event.start.value), Qt::ISODate);
        QString when = QString::fromStdString(event.start.value);
        if (start.isValid())
        {
            when = event.showWithoutTime
                       ? QLocale{}.toString(start.date(), QLocale::LongFormat)
                       : i18nc("calendar confirmation date and time", "%1 at %2",
                               QLocale{}.toString(start.date(), QLocale::LongFormat),
                               QLocale{}.toString(start.time(), QLocale::ShortFormat));
        }
        return i18n("Event: %1\nWhen: %2", title, when);
    }

    QColor automaticCalendarColor(const std::string_view ownerAccountId,
                                  const std::string_view accountId,
                                  const std::string_view calendarId, const QColor& surfaceColor)
    {
        std::uint64_t hash = 14695981039346656037ULL;
        hashCalendarIdentityPart(hash, ownerAccountId);
        hashCalendarIdentityPart(hash, accountId);
        hashCalendarIdentityPart(hash, calendarId);
        const auto hue = automaticCalendarHues[hash % automaticCalendarHues.size()];
        const bool darkSurface = surfaceColor.isValid() && surfaceColor.lightnessF() < 0.5;
        return QColor::fromHslF(static_cast<float>(hue) / 360.0F, 0.58F,
                                darkSurface ? 0.64F : 0.46F);
    }

    CalendarAccountPresentation buildCalendarAccountPresentation(
        const javelin::jmap::cache::CalendarAccount& account,
        const std::vector<javelin::jmap::calendar::Calendar>& calendars,
        const std::optional<javelin::jmap::cache::CalendarWindow>& window,
        const QColor& surfaceColor)
    {
        CalendarAccountPresentation result;
        std::unordered_map<std::string, QColor> colors;
        std::unordered_set<std::string> subscribedCalendars;
        result.calendars.reserve(calendars.size());
        for (const auto& calendar : calendars)
        {
            const auto key = account.accountId + '\n' + calendar.id;
            const auto serverColor =
                calendar.color ? QColor{QString::fromStdString(*calendar.color)} : QColor{};
            const auto displayColor =
                serverColor.isValid()
                    ? serverColor
                    : automaticCalendarColor(account.ownerAccountId, account.accountId, calendar.id,
                                             surfaceColor);
            colors.emplace(key, displayColor);
            if (calendar.isSubscribed)
                subscribedCalendars.insert(key);
            result.calendars.push_back({
                .id = key,
                .ownerAccountId = account.ownerAccountId,
                .accountId = account.accountId,
                .calendarId = calendar.id,
                .accountName = QString::fromStdString(account.name),
                .name = QString::fromStdString(calendar.name),
                .color = serverColor,
                .subscribed = calendar.isSubscribed,
                .writable = calendar.myRights.mayWriteAll || calendar.myRights.mayWriteOwn,
                .canSetColor = calendar.isSubscribed,
                .canSetDefaultAlerts = calendar.isSubscribed,
                .deletable = calendar.myRights.mayDelete,
                .defaultDestination = calendar.isDefault,
                .defaultAlertsWithTime = calendar.defaultAlertsWithTime,
                .defaultAlertsWithoutTime = calendar.defaultAlertsWithoutTime,
            });
        }
        if (!window)
            return result;

        std::unordered_map<std::string, const javelin::jmap::calendar::CalendarEvent*> events;
        for (const auto& event : window->events)
            events.emplace(event.id, &event);
        result.events.reserve(window->occurrences.size());
        for (const auto& occurrence : window->occurrences)
        {
            const auto event = events.find(occurrence.eventId);
            if (event == events.end())
                continue;
            const auto calendarId = std::ranges::find_if(
                event->second->calendarIds,
                [&account, &subscribedCalendars](const auto& item)
                {
                    return item.second &&
                           subscribedCalendars.contains(account.accountId + '\n' + item.first);
                });
            if (calendarId == event->second->calendarIds.end())
                continue;
            auto startTime = QDateTime::fromString(
                QString::fromStdString(occurrence.localStart.value), Qt::ISODate);
            auto endTime = QDateTime::fromString(QString::fromStdString(occurrence.localEnd.value),
                                                 Qt::ISODate);
            if (!endTime.isValid() || endTime <= startTime)
                endTime = startTime.addSecs(3600);
            const auto displayCalendarId = account.accountId + '\n' + calendarId->first;
            const auto color = colors.find(displayCalendarId);
            auto title = event->second->title;
            if (occurrence.recurrenceId)
            {
                const auto occurrenceOverride =
                    event->second->recurrenceOverrides.find(occurrence.recurrenceId->value);
                if (occurrenceOverride != event->second->recurrenceOverrides.end() &&
                    occurrenceOverride->second.title)
                    title = *occurrenceOverride->second.title;
            }
            result.events.push_back({
                .accountId = account.accountId,
                .calendarId = displayCalendarId,
                .eventId = event->second->id,
                .title = QString::fromStdString(title),
                .color = color == colors.end()
                             ? automaticCalendarColor(account.ownerAccountId, account.accountId,
                                                      calendarId->first, surfaceColor)
                             : color->second,
                .start = startTime,
                .end = endTime,
                .allDay = occurrence.allDay,
                .recurrenceId = occurrence.recurrenceId
                                    ? std::optional{occurrence.recurrenceId->value}
                                    : std::nullopt,
                .recurring = occurrence.recurrenceId.has_value(),
            });
        }
        return result;
    }
} // namespace javelin::gui::calendar
