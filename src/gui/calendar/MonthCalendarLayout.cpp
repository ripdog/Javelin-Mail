#include "gui/calendar/MonthCalendarLayout.h"

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
} // namespace javelin::gui::calendar
