#pragma once

#include <QColor>
#include <QDate>
#include <QDateTime>
#include <QLocale>
#include <QWidget>

#include <array>
#include <optional>
#include <string>
#include <vector>

class QLabel;
class QGridLayout;
class QToolButton;

namespace javelin::gui::calendar
{
    struct MonthEvent
    {
        std::string accountId;
        std::string calendarId;
        std::string eventId;
        QString title;
        QColor color;
        QDateTime start;
        QDateTime end;
        bool allDay = false;
        std::optional<std::string> recurrenceId;
        bool recurring = false;
    };

    struct CalendarDisplay
    {
        std::string id;
        QString name;
        QColor color;
        bool visible = true;
    };

    class DayCellWidget;

    class MonthCalendarWidget final : public QWidget
    {
        Q_OBJECT

      public:
        explicit MonthCalendarWidget(QWidget* parent = nullptr);

        void setLocale(const QLocale& locale);
        void setDisplayedMonth(const QDate& month);
        void setEvents(std::vector<MonthEvent> events);
        void setCalendars(std::vector<CalendarDisplay> calendars);
        void setHiddenCalendars(std::vector<std::string> calendarIds);

        [[nodiscard]] QDate displayedMonth() const;
        [[nodiscard]] QDate selectedDate() const;
        [[nodiscard]] QDate visibleStart() const;
        [[nodiscard]] QDate visibleEnd() const;
        [[nodiscard]] QDate cellDate(int index) const;
        [[nodiscard]] int cellCount() const;
        [[nodiscard]] int overflowCount(const QDate& date) const;

      public Q_SLOTS:
        void showPreviousMonth();
        void showNextMonth();
        void showToday();

      Q_SIGNALS:
        void visibleIntervalChanged(const QDate& start, const QDate& end);
        void selectionChanged(const QDate& date);
        void dayAgendaRequested(const QDate& date);
        void eventActivated(const QString& accountId, const QString& eventId,
                            const QString& recurrenceId);
        void emptyTimeActivated(const QDate& date);

      protected:
        void keyPressEvent(QKeyEvent* event) override;

      private:
        void rebuildDates();
        void rebuildEvents();
        void selectDate(const QDate& date, bool activate);
        void showDayAgenda(const QDate& date);

        QLocale m_locale;
        QDate m_displayedMonth;
        QDate m_selectedDate;
        QGridLayout* m_grid = nullptr;
        QLabel* m_title = nullptr;
        QToolButton* m_calendarsButton = nullptr;
        std::array<QLabel*, 7> m_weekdayHeaders{};
        std::array<DayCellWidget*, 42> m_cells{};
        std::vector<MonthEvent> m_events;
        std::vector<std::string> m_hiddenCalendars;
        std::vector<std::string> m_knownCalendars;
    };
} // namespace javelin::gui::calendar
