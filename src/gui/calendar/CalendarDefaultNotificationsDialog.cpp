#include "gui/calendar/CalendarDefaultNotificationsDialog.h"
#include "gui/calendar/CalendarNotificationEditor.h"

#include <KLocalizedString>

#include <QDialogButtonBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace javelin::gui::calendar
{
    CalendarDefaultNotificationsDialog::CalendarDefaultNotificationsDialog(
        const QString& calendarName, Alerts withTime, Alerts withoutTime,
        const bool initiallyAllDay, QWidget* parent)
        : QDialog(parent), m_originalWithTime(std::move(withTime)),
          m_originalWithoutTime(std::move(withoutTime))
    {
        setWindowTitle(i18nc("@title:window", "Default Notifications — %1", calendarName));
        resize(560, 300);
        auto* layout = new QVBoxLayout(this);
        auto* tabs = new QTabWidget(this);
        m_timed = new CalendarNotificationEditor(false, tabs);
        m_timed->setAlerts(m_originalWithTime);
        m_allDay = new CalendarNotificationEditor(false, tabs);
        m_allDay->setAlerts(m_originalWithoutTime);
        tabs->addTab(m_timed, i18nc("@title:tab", "Timed events"));
        tabs->addTab(m_allDay, i18nc("@title:tab", "All-day events"));
        tabs->setCurrentIndex(initiallyAllDay ? 1 : 0);
        layout->addWidget(tabs);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
    }

    CalendarDefaultNotificationsDialog::Alerts
    CalendarDefaultNotificationsDialog::mergeDisplayAlerts(Alerts alerts,
                                                           const CalendarNotificationEditor& editor)
    {
        std::erase_if(alerts,
                      [](const auto& entry)
                      {
                          return entry.second.action == "display" &&
                                 entry.second.triggerKind ==
                                     javelin::jmap::calendar::AlertTriggerKind::Offset;
                      });
        for (auto [alertId, alert] : editor.displayAlerts())
            alerts.insert_or_assign(std::move(alertId), std::move(alert));
        return alerts;
    }

    CalendarDefaultNotificationsDialog::Alerts
    CalendarDefaultNotificationsDialog::alertsWithTime() const
    {
        return mergeDisplayAlerts(m_originalWithTime, *m_timed);
    }

    CalendarDefaultNotificationsDialog::Alerts
    CalendarDefaultNotificationsDialog::alertsWithoutTime() const
    {
        return mergeDisplayAlerts(m_originalWithoutTime, *m_allDay);
    }
} // namespace javelin::gui::calendar
