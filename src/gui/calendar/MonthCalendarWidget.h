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
class QFocusEvent;
class QGridLayout;
class QMenu;
class QResizeEvent;
class QToolButton;

namespace javelin::gui::settings
{
    class WorkspaceSettingsPort;
}

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
        bool subscribed = false;
        bool writable = false;
        bool deletable = false;
        bool defaultDestination = false;
    };

    struct CalendarAccountDisplay
    {
        std::string id;
        QString name;
    };

    struct PendingInvitationDisplay
    {
        QString accountId;
        QString eventId;
        QString recurrenceId;
        QDate navigationDate;
        QString title;
        QString organizer;
        QDateTime displayTime;
        bool allDay = false;
    };

    class AccessibleDayCell;
    class AccessibleMonthCalendar;
    class DayCellWidget;

    class MonthCalendarWidget final : public QWidget
    {
        Q_OBJECT

      public:
        explicit MonthCalendarWidget(javelin::gui::settings::WorkspaceSettingsPort& settings,
                                     QWidget* parent = nullptr);

        void setLocale(const QLocale& locale);
        void setDisplayedMonth(const QDate& month);
        void setEvents(std::vector<MonthEvent> events);
        void setCalendars(std::vector<CalendarDisplay> calendars);
        void setCalendarAccounts(std::vector<CalendarAccountDisplay> accounts);
        void setPendingInvitations(std::vector<PendingInvitationDisplay> invitations);
        void applicationPaletteChanged();

        [[nodiscard]] QDate displayedMonth() const;
        [[nodiscard]] QDate selectedDate() const;
        [[nodiscard]] QDate visibleStart() const;
        [[nodiscard]] QDate visibleEnd() const;
        [[nodiscard]] QDate cellDate(int index) const;
        [[nodiscard]] int cellCount() const;
        [[nodiscard]] int overflowCount(const QDate& date) const;
        [[nodiscard]] std::vector<MonthEvent> eventsForDate(const QDate& date) const;
        [[nodiscard]] QColor calendarColor(const QString& calendarId) const;
        [[nodiscard]] QMenu* calendarMenu() const;

      public Q_SLOTS:
        void createEvent();
        void manageCalendars();
        void showPreviousMonth();
        void showNextMonth();
        void showToday();
        void setSelectedDateFromAgenda(const QDate& date);

      Q_SIGNALS:
        void visibleIntervalChanged(const QDate& start, const QDate& end);
        void selectionChanged(const QDate& date);
        void dayAgendaRequested(const QDate& date, const QString& accountId, const QString& eventId,
                                const QString& recurrenceId);
        void eventActivated(const QString& accountId, const QString& eventId,
                            const QString& recurrenceId);
        void eventEditRequested(const QString& accountId, const QString& eventId,
                                const QString& recurrenceId);
        void pendingInvitationActivated(const QString& accountId, const QString& eventId,
                                        const QString& recurrenceId, const QDate& navigationDate);
        void eventContextMenuRequested(const QPoint& globalPosition, const QString& accountId,
                                       const QString& eventId, const QString& recurrenceId);
        void eventContextActionRequested(const QString& actionId, const QString& accountId,
                                         const QString& eventId, const QString& recurrenceId,
                                         const QString& targetCalendarId);
        void emptyTimeActivated(const QDateTime& start, const QDateTime& end);
        void calendarSubscriptionChanged(const QString& calendarId, bool subscribed);
        void defaultCalendarChanged(const QString& calendarId);
        void calendarCreationRequested(const QString& accountId, const QString& name,
                                       const QString& color);
        void calendarDeletionRequested(const QString& calendarId);

      protected:
        void keyPressEvent(QKeyEvent* event) override;
        void focusInEvent(QFocusEvent* event) override;
        void resizeEvent(QResizeEvent* event) override;
        void changeEvent(QEvent* event) override;

      private:
        friend class AccessibleDayCell;
        friend class AccessibleMonthCalendar;

        void rebuildDates();
        void rebuildEvents();
        void rebuildCalendarMenu();
        void applyCalendarColors();
        [[nodiscard]] QColor effectiveCalendarColor(const std::string& calendarId) const;
        void selectDate(const QDate& date);
        void scheduleEventRebuild();
        void reloadCalendarColors();
        [[nodiscard]] DayCellWidget* cellForDate(const QDate& date) const;
        void notifyAccessibilityGridChanged();
        void notifyAccessibilitySelectionChanged();

        javelin::gui::settings::WorkspaceSettingsPort& m_settings;
        QLocale m_locale;
        QDate m_displayedMonth;
        QDate m_selectedDate;
        QGridLayout* m_grid = nullptr;
        QLabel* m_title = nullptr;
        QWidget* m_invitationBanner = nullptr;
        QLabel* m_invitationLabel = nullptr;
        QToolButton* m_viewInvitations = nullptr;
        QMenu* m_invitationMenu = nullptr;
        QMenu* m_calendarMenu = nullptr;
        std::array<QLabel*, 7> m_weekdayHeaders{};
        std::array<DayCellWidget*, 42> m_cells{};
        std::vector<CalendarDisplay> m_calendars;
        std::vector<CalendarAccountDisplay> m_calendarAccounts;
        std::vector<MonthEvent> m_events;
        std::unordered_map<std::string, QColor> m_customCalendarColors;
        bool m_eventRebuildPending = false;
    };
} // namespace javelin::gui::calendar
