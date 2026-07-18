#include "gui/calendar/RecurrencePattern.h"

#include <algorithm>
#include <string>

namespace
{
    using javelin::jmap::calendar::RecurrenceFrequency;
    using javelin::jmap::calendar::RecurrenceRule;
    using javelin::jmap::calendar::RecurrenceSkip;
    using javelin::jmap::calendar::Weekday;

    bool hasAdvancedSelectors(const RecurrenceRule& rule)
    {
        return !rule.byYearDay.empty() || !rule.byWeekNo.empty() || !rule.byHour.empty() ||
               !rule.byMinute.empty() || !rule.bySecond.empty() || !rule.bySetPosition.empty() ||
               (rule.rscale && *rule.rscale != "gregorian") ||
               (rule.skip && *rule.skip != RecurrenceSkip::Omit);
    }
} // namespace

namespace javelin::gui::calendar
{
    javelin::jmap::calendar::Weekday weekday(const Qt::DayOfWeek day)
    {
        switch (day)
        {
        case Qt::Monday:
            return Weekday::Monday;
        case Qt::Tuesday:
            return Weekday::Tuesday;
        case Qt::Wednesday:
            return Weekday::Wednesday;
        case Qt::Thursday:
            return Weekday::Thursday;
        case Qt::Friday:
            return Weekday::Friday;
        case Qt::Saturday:
            return Weekday::Saturday;
        case Qt::Sunday:
            return Weekday::Sunday;
        }
        return Weekday::Monday;
    }

    Qt::DayOfWeek qtWeekday(const Weekday day)
    {
        switch (day)
        {
        case Weekday::Monday:
            return Qt::Monday;
        case Weekday::Tuesday:
            return Qt::Tuesday;
        case Weekday::Wednesday:
            return Qt::Wednesday;
        case Weekday::Thursday:
            return Qt::Thursday;
        case Weekday::Friday:
            return Qt::Friday;
        case Weekday::Saturday:
            return Qt::Saturday;
        case Weekday::Sunday:
            return Qt::Sunday;
        }
        return Qt::Monday;
    }

    int ordinalWeekday(const QDate& date)
    {
        if (!date.isValid())
            return 1;
        return date.addDays(7).month() == date.month() ? (date.day() - 1) / 7 + 1 : -1;
    }

    FriendlyRecurrencePattern friendlyRecurrencePattern(const RecurrenceRule& rule,
                                                        const QDateTime& eventStart)
    {
        FriendlyRecurrencePattern pattern;
        pattern.interval = std::max<std::uint32_t>(1, rule.interval);
        pattern.firstDayOfWeek = rule.firstDayOfWeek;
        if (rule.count)
        {
            pattern.end = FriendlyRecurrenceEnd::AfterCount;
            pattern.count = std::max<std::uint32_t>(1, *rule.count);
        }
        else if (rule.until)
        {
            const auto until =
                QDateTime::fromString(QString::fromStdString(rule.until->value), Qt::ISODate);
            if (until.isValid())
            {
                pattern.end = FriendlyRecurrenceEnd::OnDate;
                pattern.untilDate = until.date();
            }
            else
            {
                pattern.replacesUnsupportedRule = true;
            }
        }

        switch (rule.frequency)
        {
        case RecurrenceFrequency::Daily:
            pattern.frequency = FriendlyRecurrenceFrequency::Day;
            pattern.replacesUnsupportedRule |=
                !rule.byDay.empty() || !rule.byMonthDay.empty() || !rule.byMonth.empty();
            break;
        case RecurrenceFrequency::Weekly:
            pattern.frequency = FriendlyRecurrenceFrequency::Week;
            pattern.replacesUnsupportedRule |= !rule.byMonthDay.empty() || !rule.byMonth.empty();
            for (const auto& day : rule.byDay)
            {
                if (day.nthOfPeriod)
                    pattern.replacesUnsupportedRule = true;
                else
                    pattern.weekdays.push_back(day.day);
            }
            if (pattern.weekdays.empty() && eventStart.isValid())
                pattern.weekdays.push_back(
                    weekday(static_cast<Qt::DayOfWeek>(eventStart.date().dayOfWeek())));
            break;
        case RecurrenceFrequency::Monthly:
            pattern.frequency = FriendlyRecurrenceFrequency::Month;
            if (rule.byMonth.empty() && rule.byMonthDay.size() == 1 && rule.byDay.empty() &&
                eventStart.isValid() && rule.byMonthDay.front() == eventStart.date().day())
            {
                pattern.monthlyMode = FriendlyMonthlyMode::DayOfMonth;
            }
            else if (rule.byMonth.empty() && rule.byMonthDay.empty() && rule.byDay.size() == 1 &&
                     rule.byDay.front().nthOfPeriod && eventStart.isValid() &&
                     rule.byDay.front().day ==
                         weekday(static_cast<Qt::DayOfWeek>(eventStart.date().dayOfWeek())) &&
                     *rule.byDay.front().nthOfPeriod == ordinalWeekday(eventStart.date()))
            {
                pattern.monthlyMode = FriendlyMonthlyMode::OrdinalWeekday;
            }
            else if (rule.byMonth.empty() && rule.byMonthDay.empty() && rule.byDay.empty())
            {
                pattern.monthlyMode = FriendlyMonthlyMode::DayOfMonth;
            }
            else
            {
                pattern.replacesUnsupportedRule = true;
            }
            break;
        case RecurrenceFrequency::Yearly:
            pattern.frequency = FriendlyRecurrenceFrequency::Year;
            if (!eventStart.isValid() || !rule.byDay.empty() || !rule.byYearDay.empty() ||
                !rule.byWeekNo.empty() || rule.byMonth.size() > 1 || rule.byMonthDay.size() > 1 ||
                (rule.byMonth.size() == 1 &&
                 rule.byMonth.front() != std::to_string(eventStart.date().month())) ||
                (rule.byMonthDay.size() == 1 && rule.byMonthDay.front() != eventStart.date().day()))
                pattern.replacesUnsupportedRule = true;
            break;
        case RecurrenceFrequency::Hourly:
        case RecurrenceFrequency::Minutely:
        case RecurrenceFrequency::Secondly:
            pattern.frequency = FriendlyRecurrenceFrequency::Day;
            pattern.replacesUnsupportedRule = true;
            break;
        }
        pattern.replacesUnsupportedRule =
            pattern.replacesUnsupportedRule || hasAdvancedSelectors(rule);
        return pattern;
    }

    RecurrenceRule recurrenceRule(const FriendlyRecurrencePattern& pattern,
                                  const QDateTime& eventStart)
    {
        RecurrenceRule rule;
        rule.interval = std::max<std::uint32_t>(1, pattern.interval);
        rule.firstDayOfWeek = pattern.firstDayOfWeek;
        switch (pattern.frequency)
        {
        case FriendlyRecurrenceFrequency::Day:
            rule.frequency = RecurrenceFrequency::Daily;
            break;
        case FriendlyRecurrenceFrequency::Week:
            rule.frequency = RecurrenceFrequency::Weekly;
            for (const auto day : pattern.weekdays)
                rule.byDay.push_back({.day = day, .nthOfPeriod = std::nullopt});
            break;
        case FriendlyRecurrenceFrequency::Month:
            rule.frequency = RecurrenceFrequency::Monthly;
            if (eventStart.isValid())
            {
                if (pattern.monthlyMode == FriendlyMonthlyMode::DayOfMonth)
                    rule.byMonthDay.push_back(eventStart.date().day());
                else
                    rule.byDay.push_back(
                        {.day = weekday(static_cast<Qt::DayOfWeek>(eventStart.date().dayOfWeek())),
                         .nthOfPeriod = ordinalWeekday(eventStart.date())});
            }
            break;
        case FriendlyRecurrenceFrequency::Year:
            rule.frequency = RecurrenceFrequency::Yearly;
            break;
        }
        if (pattern.end == FriendlyRecurrenceEnd::AfterCount)
            rule.count = std::max<std::uint32_t>(1, pattern.count);
        else if (pattern.end == FriendlyRecurrenceEnd::OnDate && pattern.untilDate &&
                 eventStart.isValid())
            rule.until = javelin::jmap::calendar::LocalDateTime{
                .value = QDateTime{*pattern.untilDate, eventStart.time()}
                             .toString(Qt::ISODate)
                             .toStdString()};
        return rule;
    }
} // namespace javelin::gui::calendar
