#include "gui/calendar/CalendarPresentation.h"

#include <QDateTime>

#include <ranges>
#include <unordered_map>

namespace javelin::gui::calendar
{
    CalendarAccountPresentation buildCalendarAccountPresentation(
        const javelin::jmap::cache::CalendarAccount& account,
        const std::vector<javelin::jmap::calendar::Calendar>& calendars,
        const std::optional<javelin::jmap::cache::CalendarWindow>& window,
        const QColor& fallbackColor)
    {
        CalendarAccountPresentation result;
        std::unordered_map<std::string, QColor> colors;
        result.calendars.reserve(calendars.size());
        for (const auto& calendar : calendars)
        {
            const auto key = account.accountId + '\n' + calendar.id;
            const auto color =
                calendar.color ? QColor{QString::fromStdString(*calendar.color)} : fallbackColor;
            colors.emplace(key, color);
            result.calendars.push_back({
                .id = key,
                .accountId = account.accountId,
                .accountName = QString::fromStdString(account.name),
                .name = QStringLiteral("%1 — %2").arg(QString::fromStdString(calendar.name),
                                                      QString::fromStdString(account.name)),
                .color = color,
                .visible = calendar.isVisible,
                .writable = calendar.myRights.mayWriteAll || calendar.myRights.mayWriteOwn,
                .deletable = calendar.myRights.mayDelete,
                .defaultDestination = calendar.isDefault,
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
                event->second->calendarIds, [&account, &colors](const auto& item)
                { return item.second && colors.contains(account.accountId + '\n' + item.first); });
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
                .color = color == colors.end() ? fallbackColor : color->second,
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
