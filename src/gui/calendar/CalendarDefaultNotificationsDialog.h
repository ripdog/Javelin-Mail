#pragma once

#include "jmap/calendar/CalendarTypes.h"

#include <QDialog>

#include <string>
#include <unordered_map>

namespace javelin::gui::calendar
{
    class CalendarNotificationEditor;

    class CalendarDefaultNotificationsDialog final : public QDialog
    {
        Q_OBJECT

      public:
        using Alerts = std::unordered_map<std::string, javelin::jmap::calendar::Alert>;

        CalendarDefaultNotificationsDialog(const QString& calendarName, Alerts withTime,
                                           Alerts withoutTime, bool initiallyAllDay,
                                           QWidget* parent = nullptr);

        [[nodiscard]] Alerts alertsWithTime() const;
        [[nodiscard]] Alerts alertsWithoutTime() const;

      private:
        [[nodiscard]] static Alerts mergeDisplayAlerts(Alerts alerts,
                                                       const CalendarNotificationEditor& editor);

        Alerts m_originalWithTime;
        Alerts m_originalWithoutTime;
        CalendarNotificationEditor* m_timed = nullptr;
        CalendarNotificationEditor* m_allDay = nullptr;
    };
} // namespace javelin::gui::calendar
