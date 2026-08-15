#pragma once

#include "jmap/calendar/CalendarTypes.h"

#include <QDialog>

#include <vector>

class QCheckBox;
class QPushButton;
class QComboBox;
class QDateEdit;
class QDateTimeEdit;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QVBoxLayout;
class QSpinBox;

namespace javelin::gui::widgets
{
    class EmailAddressLineEdit;
}

namespace javelin::gui::calendar
{
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

        struct AlertRow
        {
            QWidget* container = nullptr;
            std::string id;
            javelin::jmap::calendar::Alert original;
            QComboBox* triggerKind = nullptr;
            QSpinBox* amount = nullptr;
            QComboBox* unit = nullptr;
            QComboBox* relation = nullptr;
            QDateTimeEdit* absoluteTime = nullptr;
            QPushButton* remove = nullptr;
            bool edited = false;
        };

        void addAttendeeRow(const QString& address = {});
        void removeAttendeeRow(QWidget* row);
        void clearAttendeeRows();
        void addAlertRow(const std::optional<javelin::jmap::calendar::Alert>& alert = std::nullopt);
        void removeAlertRow(QWidget* row);
        void clearAlertRows();
        void updateAlertRow(AlertRow& row);
        void markAlertEdited(QWidget* row);
        void updateRecurrenceControls();

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
        QCheckBox* m_useDefaultAlerts = nullptr;
        QWidget* m_alerts = nullptr;
        QVBoxLayout* m_alertRowsLayout = nullptr;
        QPushButton* m_addAlert = nullptr;
        std::vector<AlertRow> m_alertRows;
        QLabel* m_error = nullptr;
        QPushButton* m_delete = nullptr;
        bool m_endEdited = false;
        bool m_calendarEdited = false;
        bool m_timeZoneEdited = false;
        bool m_recurrenceEdited = false;
        bool m_attendeesEdited = false;
        bool m_alertsEdited = false;
        bool m_occurrenceMode = false;
    };
} // namespace javelin::gui::calendar
