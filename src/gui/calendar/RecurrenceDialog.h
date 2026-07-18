#pragma once

#include "jmap/calendar/CalendarTypes.h"

#include <QDialog>

#include <vector>

class QComboBox;
class QDateTimeEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QVBoxLayout;

namespace javelin::gui::calendar
{
    class RecurrenceDialog final : public QDialog
    {
        Q_OBJECT

      public:
        explicit RecurrenceDialog(QWidget* parent = nullptr);

        void setRule(const javelin::jmap::calendar::RecurrenceRule& rule);
        [[nodiscard]] javelin::jmap::calendar::RecurrenceRule rule() const;

      private Q_SLOTS:
        void validateAndAccept();
        void updateEndMode();

      private:
        struct DayRow
        {
            QWidget* container = nullptr;
            QComboBox* day = nullptr;
            QSpinBox* ordinal = nullptr;
            QPushButton* remove = nullptr;
        };

        void
        addDayRow(const std::optional<javelin::jmap::calendar::RecurrenceDay>& day = std::nullopt);
        void removeDayRow(QWidget* row);
        void clearDayRows();
        [[nodiscard]] std::optional<javelin::jmap::calendar::RecurrenceRule>
        validatedRule(QString& error) const;

        QComboBox* m_frequency = nullptr;
        QSpinBox* m_interval = nullptr;
        QLineEdit* m_rscale = nullptr;
        QComboBox* m_skip = nullptr;
        QComboBox* m_firstDay = nullptr;
        QWidget* m_dayRowsWidget = nullptr;
        QVBoxLayout* m_dayRowsLayout = nullptr;
        std::vector<DayRow> m_dayRows;
        QLineEdit* m_byMonthDay = nullptr;
        QLineEdit* m_byMonth = nullptr;
        QLineEdit* m_byYearDay = nullptr;
        QLineEdit* m_byWeekNo = nullptr;
        QLineEdit* m_byHour = nullptr;
        QLineEdit* m_byMinute = nullptr;
        QLineEdit* m_bySecond = nullptr;
        QLineEdit* m_bySetPosition = nullptr;
        QComboBox* m_endMode = nullptr;
        QSpinBox* m_count = nullptr;
        QDateTimeEdit* m_until = nullptr;
        QLabel* m_error = nullptr;
        javelin::jmap::calendar::RecurrenceRule m_rule;
    };
} // namespace javelin::gui::calendar
