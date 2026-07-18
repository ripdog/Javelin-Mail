#pragma once

#include "gui/calendar/RecurrencePattern.h"

#include <QDialog>

#include <utility>
#include <vector>

class QComboBox;
class QDateEdit;
class QLabel;
class QRadioButton;
class QSpinBox;
class QToolButton;

namespace javelin::gui::calendar
{
    class RecurrenceDialog final : public QDialog
    {
        Q_OBJECT

      public:
        explicit RecurrenceDialog(QWidget* parent = nullptr);

        void setEventStart(const QDateTime& start);
        void setRule(const javelin::jmap::calendar::RecurrenceRule& rule);
        [[nodiscard]] javelin::jmap::calendar::RecurrenceRule rule() const;

      private Q_SLOTS:
        void validateAndAccept();
        void updateFrequencyControls();
        void updateEndControls();

      private:
        void updateMonthlyChoices();
        [[nodiscard]] FriendlyRecurrencePattern pattern() const;

        QDateTime m_eventStart;
        javelin::jmap::calendar::RecurrenceRule m_rule;
        std::optional<javelin::jmap::calendar::Weekday> m_firstDayOfWeek;
        QLabel* m_unsupported = nullptr;
        QSpinBox* m_interval = nullptr;
        QComboBox* m_frequency = nullptr;
        QWidget* m_weeklyControls = nullptr;
        std::vector<std::pair<javelin::jmap::calendar::Weekday, QToolButton*>> m_weekdays;
        QComboBox* m_monthlyMode = nullptr;
        QRadioButton* m_never = nullptr;
        QRadioButton* m_onDate = nullptr;
        QDateEdit* m_untilDate = nullptr;
        QRadioButton* m_afterCount = nullptr;
        QSpinBox* m_count = nullptr;
        QLabel* m_error = nullptr;
    };
} // namespace javelin::gui::calendar
