#pragma once

#include <QColor>
#include <QDate>
#include <QDateTime>
#include <QLocale>
#include <QWidget>

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class QLabel;
class QGridLayout;
class QMenu;
class QResizeEvent;

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
        std::string accountId;
        QString accountName;
        QString name;
        QColor color;
        bool visible = true;
        bool writable = false;
        bool deletable = false;
        bool defaultDestination = false;
    };

    struct CalendarAccountDisplay
    {
        std::string id;
        QString name;
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
        void setCalendarAccounts(std::vector<CalendarAccountDisplay> accounts);
        void setHiddenCalendars(std::vector<std::string> calendarIds);
        void applicationPaletteChanged();

        [[nodiscard]] QDate displayedMonth() const;
        [[nodiscard]] QDate selectedDate() const;
        [[nodiscard]] QDate visibleStart() const;
        [[nodiscard]] QDate visibleEnd() const;
        [[nodiscard]] QDate cellDate(int index) const;
        [[nodiscard]] int cellCount() const;
        [[nodiscard]] int overflowCount(const QDate& date) const;
        [[nodiscard]] QMenu* calendarMenu() const;

      public Q_SLOTS:
        void createEvent();
        void manageCalendars();
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
        void calendarVisibilityChanged(const QString& calendarId, bool visible);
        void defaultCalendarChanged(const QString& calendarId);
        void calendarCreationRequested(const QString& accountId, const QString& name,
                                       const QString& color);
        void calendarDeletionRequested(const QString& calendarId);

      protected:
        void keyPressEvent(QKeyEvent* event) override;
        void resizeEvent(QResizeEvent* event) override;
        void changeEvent(QEvent* event) override;

      private:
        void rebuildDates();
        void rebuildEvents();
        void rebuildCalendarMenu();
        void applyCalendarColors();
        [[nodiscard]] QColor effectiveCalendarColor(const std::string& calendarId) const;
        void selectDate(const QDate& date, bool activate);
        void showDayAgenda(const QDate& date);
        void scheduleEventRebuild();

        QLocale m_locale;
        QDate m_displayedMonth;
        QDate m_selectedDate;
        QGridLayout* m_grid = nullptr;
        QLabel* m_title = nullptr;
        QMenu* m_calendarMenu = nullptr;
        std::array<QLabel*, 7> m_weekdayHeaders{};
        std::array<DayCellWidget*, 42> m_cells{};
        std::vector<CalendarDisplay> m_calendars;
        std::vector<CalendarAccountDisplay> m_calendarAccounts;
        std::vector<MonthEvent> m_events;
        std::vector<std::string> m_hiddenCalendars;
        std::vector<std::string> m_knownCalendars;
        std::unordered_map<std::string, QColor> m_customCalendarColors;
        bool m_eventRebuildPending = false;
    };
} // namespace javelin::gui::calendar
