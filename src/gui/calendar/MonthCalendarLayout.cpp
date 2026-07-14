#include "gui/calendar/MonthCalendarLayout.h"

#include <algorithm>
#include <utility>

namespace javelin::gui::calendar
{
    QDate monthGridStart(const QDate& month, const QLocale& locale)
    {
        const QDate first{month.year(), month.month(), 1};
        const auto firstDay = static_cast<int>(locale.firstDayOfWeek());
        const auto offset = (first.dayOfWeek() - firstDay + 7) % 7;
        return first.addDays(-offset);
    }

    QDate monthGridCellDate(const QDate& month, const QLocale& locale, const int index)
    {
        if (!month.isValid() || index < 0 || index >= 42)
            return {};
        return monthGridStart(month, locale).addDays(index);
    }

    std::size_t monthCellVisibleEventCount(const int cellHeight, const int headerHeight,
                                           const int eventRowHeight, const int verticalMargins,
                                           const int spacing, const std::size_t eventCount)
    {
        if (eventCount == 0)
            return 0;
        const auto rowStride = std::max(1, eventRowHeight + spacing);
        const auto available = std::max(0, cellHeight - headerHeight - verticalMargins - spacing);
        const auto rowCount = std::max(1, available / rowStride);
        const auto slots = static_cast<std::size_t>(rowCount);
        if (eventCount <= slots)
            return eventCount;
        return slots > 1 ? slots - 1 : 0;
    }

    QDate monthEventLastDate(const QDateTime& start, const QDateTime& end)
    {
        if (end.time() == QTime{0, 0} && end.date() > start.date())
            return end.date().addDays(-1);
        return end.date();
    }

    MonthEventSegment monthEventSegment(const QString& title, const QDateTime& start,
                                        const QDateTime& end, const bool allDay,
                                        const QDate& cellDate)
    {
        const auto firstDate = start.date();
        const auto lastDate = monthEventLastDate(start, end);
        const auto begins = cellDate == firstDate;
        const auto ends = cellDate == lastDate;
        auto label = title;
        if (!allDay && begins)
            label.prepend(start.time().toString(QStringLiteral("HH:mm ")));
        if (!begins)
            label.prepend(QStringLiteral("← "));
        if (!ends)
            label.append(QStringLiteral(" →"));
        return {.label = std::move(label), .begins = begins, .ends = ends};
    }
} // namespace javelin::gui::calendar
