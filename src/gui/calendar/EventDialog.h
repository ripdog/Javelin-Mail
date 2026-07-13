#pragma once

#include "jmap/calendar/CalendarTypes.h"

#include <QDialog>

#include <vector>

class QCheckBox;
class QPushButton;
class QComboBox;
class QDateEdit;
class QLabel;
class QLineEdit;
class QPlainTextEdit;

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
        [[nodiscard]] javelin::jmap::calendar::CalendarEvent eventDocument() const;
        void showMutationError(const QString& message);

      private Q_SLOTS:
        void validateAndAccept();
        void updateAllDayMode(bool allDay);
        void updateAutomaticEnd();
        void markEndEdited();

      private:
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
        QPlainTextEdit* m_attendees = nullptr;
        QLabel* m_error = nullptr;
        QPushButton* m_delete = nullptr;
        bool m_endEdited = false;
    };
} // namespace javelin::gui::calendar
