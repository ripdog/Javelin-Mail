#include "gui/calendar/CalendarPresentation.h"

#include <QDateTime>

#include <ranges>
#include <unordered_map>
#include <unordered_set>

namespace javelin::gui::calendar
{
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

    CalendarAccountPresentation buildCalendarAccountPresentation(
        const javelin::jmap::cache::CalendarAccount& account,
        const std::vector<javelin::jmap::calendar::Calendar>& calendars,
        const std::optional<javelin::jmap::cache::CalendarWindow>& window,
        const QColor& fallbackColor)
    {
        CalendarAccountPresentation result;
        std::unordered_map<std::string, QColor> colors;
        std::unordered_set<std::string> subscribedCalendars;
        result.calendars.reserve(calendars.size());
        for (const auto& calendar : calendars)
        {
            const auto key = account.accountId + '\n' + calendar.id;
            const auto color =
                calendar.color ? QColor{QString::fromStdString(*calendar.color)} : fallbackColor;
            colors.emplace(key, color);
            if (calendar.isSubscribed)
                subscribedCalendars.insert(key);
            result.calendars.push_back({
                .id = key,
                .ownerAccountId = account.ownerAccountId,
                .accountId = account.accountId,
                .calendarId = calendar.id,
                .accountName = QString::fromStdString(account.name),
                .name = QString::fromStdString(calendar.name),
                .color = color,
                .subscribed = calendar.isSubscribed,
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
