#pragma once

#include "jmap/calendar/CalendarTypes.h"

#include <QDialog>

#include <vector>

class QAction;
class QCheckBox;
class QPushButton;
class QComboBox;
class QDateEdit;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QVBoxLayout;
class KMessageWidget;

namespace javelin::gui::widgets
{
    class EmailAddressLineEdit;
}

namespace javelin::gui::calendar
{
    class CalendarNotificationEditor;

    class EventDialog final : public QDialog
    {
        Q_OBJECT

      public:
        static constexpr int DeleteRequested = QDialog::Accepted + 1;

        EventDialog(std::vector<javelin::jmap::calendar::Calendar> calendars,
                    QWidget* parent = nullptr);

        void setEvent(const javelin::jmap::calendar::CalendarEvent& event);
        void setOccurrenceMode(bool occurrenceMode);
        [[nodiscard]] javelin::jmap::calendar::CalendarEvent eventDocument() const;
        void showMutationError(const QString& message);
        void completeDefaultNotificationsChange(const QString& accountId, const QString& calendarId,
                                                bool success, const QString& errorMessage = {});

      Q_SIGNALS:
        void defaultNotificationsChangeRequested(
            QString accountId, QString calendarId,
            std::unordered_map<std::string, javelin::jmap::calendar::Alert> withTime,
            std::unordered_map<std::string, javelin::jmap::calendar::Alert> withoutTime);

      private Q_SLOTS:
        void validateAndAccept();
        void updateAllDayMode(bool allDay);
        void updateAutomaticEnd();
        void markEndEdited();
        void editCustomRecurrence();

      private:
        struct AttendeeRow
        {
            QWidget* container = nullptr;
            javelin::gui::widgets::EmailAddressLineEdit* editor = nullptr;
            QPushButton* remove = nullptr;
        };

        void addAttendeeRow(const QString& address = {});
        void removeAttendeeRow(QWidget* row);
        void clearAttendeeRows();
        void updateRecurrenceControls();
        void updateDefaultNotificationsWarning();
        void editCurrentCalendarDefaultNotifications();
        [[nodiscard]] javelin::jmap::calendar::Calendar* currentCalendar();
        [[nodiscard]] const javelin::jmap::calendar::Calendar* currentCalendar() const;

        struct PendingDefaultNotificationsChange
        {
            QString accountId;
            QString calendarId;
            std::unordered_map<std::string, javelin::jmap::calendar::Alert> originalWithTime;
            std::unordered_map<std::string, javelin::jmap::calendar::Alert> originalWithoutTime;
        };

        std::vector<javelin::jmap::calendar::Calendar> m_calendars;
        javelin::jmap::calendar::CalendarEvent m_event;
        QLineEdit* m_title = nullptr;
        QComboBox* m_calendar = nullptr;
        QCheckBox* m_allDay = nullptr;
        QDateEdit* m_startDate = nullptr;
        QComboBox* m_startTime = nullptr;
        QDateEdit* m_endDate = nullptr;
        QComboBox* m_endTime = nullptr;
        QComboBox* m_timeZone = nullptr;
        QPlainTextEdit* m_description = nullptr;
        QLineEdit* m_location = nullptr;
        QComboBox* m_recurrence = nullptr;
        QPushButton* m_customizeRecurrence = nullptr;
        std::optional<javelin::jmap::calendar::RecurrenceRule> m_customRecurrence;
        QString m_committedRecurrenceKey = QStringLiteral("none");
        QWidget* m_attendees = nullptr;
        QVBoxLayout* m_attendeeRowsLayout = nullptr;
        std::vector<AttendeeRow> m_attendeeRows;
        CalendarNotificationEditor* m_notifications = nullptr;
        KMessageWidget* m_defaultNotificationsWarning = nullptr;
        QAction* m_configureDefaultNotifications = nullptr;
        std::optional<PendingDefaultNotificationsChange> m_pendingDefaultNotificationsChange;
        QLabel* m_error = nullptr;
        QPushButton* m_delete = nullptr;
        bool m_endEdited = false;
        bool m_calendarEdited = false;
        bool m_timeZoneEdited = false;
        bool m_recurrenceEdited = false;
        bool m_attendeesEdited = false;
        bool m_occurrenceMode = false;
    };
} // namespace javelin::gui::calendar
