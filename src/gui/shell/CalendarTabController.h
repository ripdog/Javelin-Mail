#pragma once

#include "app/CalendarApplicationPorts.h"
#include "gui/shell/TabWorkspace.h"
#include "jmap/OperationError.h"
#include "jmap/cache/CalendarRepository.h"

#include <QCoroTask>

#include <QDate>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class QAction;
class QMenu;
class QStackedWidget;
class QWidget;

namespace javelin::gui::calendar
{
    class EventDialog;
    class MonthCalendarWidget;
} // namespace javelin::gui::calendar

namespace javelin::gui::settings
{
    class GuiSettings;
}

namespace javelin::gui::shell
{
    struct CalendarEventContextActions
    {
        QAction& edit;
        QAction& duplicate;
        QAction& move;
        QAction& accept;
        QAction& tentative;
        QAction& decline;
        QAction& copyDetails;
        QAction& deleteEvent;
    };

    enum class CalendarTabCommand
    {
        CreateEvent,
        PreviousMonth,
        Today,
        GoToMonth,
        NextMonth,
        ManageCalendars,
        Refresh,
    };

    struct CalendarWorkspaceState
    {
        bool available = false;
        bool canCreateEvent = false;
        bool canManageCalendars = false;
        bool canRefresh = false;

        friend bool operator==(const CalendarWorkspaceState&,
                               const CalendarWorkspaceState&) = default;
    };

    struct CalendarViewSnapshot
    {
        QDate start;
        QDate end;
        std::unordered_map<std::string, javelin::jmap::cache::CalendarWindow> windows;
        std::optional<std::vector<javelin::jmap::calendar::PendingCalendarInvitation>>
            pendingInvitations;
    };

    class CalendarTabController final : public QObject
    {
        Q_OBJECT

      public:
        CalendarTabController(javelin::gui::settings::GuiSettings& settings,
                              javelin::app::CalendarReadPort& calendarReader,
                              javelin::app::CalendarCommandPort& calendarCommandPort,
                              QStackedWidget& contentStack, std::vector<TabState>& tabs,
                              QObject* parent = nullptr);

        void open(std::optional<QDate> displayedMonth = std::nullopt);
        void openEvent(const QString& calendarAccountId, const QString& eventId,
                       const QString& recurrenceId, const QDate& navigationDate);
        void invoke(const TabState* tab, CalendarTabCommand command);
        void invokeWorkspace(CalendarTabCommand command);
        [[nodiscard]] const CalendarWorkspaceState& workspaceState() const;
        [[nodiscard]] bool refresh(const TabState* tab);
        void accountsChanged();
        // Completes once calendar accounts have been read at least once.
        [[nodiscard]] QCoro::Task<> ensureAccountsLoaded();
        [[nodiscard]] bool close(TabState& tab);
        [[nodiscard]] QWidget* contentWidgetForTab(const TabState* tab) const;
        [[nodiscard]] QMenu* calendarMenuForTab(const TabState* tab) const;
        void applicationPaletteChanged();
        void configureEventContextMenu(std::function<std::vector<QString>()> configuredLayout,
                                       CalendarEventContextActions actions);

      Q_SIGNALS:
        void tabReady(int index);
        void workspaceStateChanged();
        void statusMessage(QString message, int durationMilliseconds);
        void operationFailed(javelin::jmap::OperationError error);

      private:
        enum class AccountSnapshotState
        {
            Unloaded,
            Loaded,
            Failed,
        };

        struct CalendarEventTarget
        {
            QString accountId;
            QString eventId;
            QString recurrenceId;
        };

        struct PendingCalendarOpen
        {
            std::optional<QDate> displayedMonth;
            std::optional<CalendarEventTarget> event;
        };

        struct PendingDayAgenda
        {
            QDate date;
            CalendarEventTarget event;
        };

        [[nodiscard]] CalendarWorkspaceState calculateWorkspaceState() const;
        void setWorkspaceState(CalendarWorkspaceState state);
        [[nodiscard]] javelin::gui::calendar::MonthCalendarWidget*
        widgetForTab(const TabState* tab) const;
        [[nodiscard]] javelin::gui::calendar::MonthCalendarWidget* calendarWidget() const;
        [[nodiscard]] QCoro::Task<> loadAccountSnapshot();
        [[nodiscard]] QCoro::Task<>
        refreshAccountsAndRange(QPointer<javelin::gui::calendar::MonthCalendarWidget> widget);
        void showCalendar(PendingCalendarOpen request);
        void createCalendarTab(PendingCalendarOpen request);
        void focusRequest(javelin::gui::calendar::MonthCalendarWidget& widget,
                          const PendingCalendarOpen& request);
        void showCalendarAccounts(javelin::gui::calendar::MonthCalendarWidget& widget) const;
        void reloadView(javelin::gui::calendar::MonthCalendarWidget& widget);
        [[nodiscard]] QCoro::Task<>
        loadView(QPointer<javelin::gui::calendar::MonthCalendarWidget> widget);
        void presentView(javelin::gui::calendar::MonthCalendarWidget& widget);
        [[nodiscard]] bool
        viewMatches(const javelin::gui::calendar::MonthCalendarWidget& widget) const;
        [[nodiscard]] const std::vector<javelin::jmap::calendar::Calendar>*
        calendarsFor(const std::string& accountId) const;
        [[nodiscard]] const std::vector<javelin::jmap::calendar::ParticipantIdentity>*
        identitiesFor(const std::string& accountId) const;
        [[nodiscard]] const javelin::jmap::cache::CalendarWindow*
        windowFor(const std::string& accountId) const;
        void requestVisibleRange(javelin::gui::calendar::MonthCalendarWidget& widget,
                                 const QDate& start, const QDate& end, bool forceRefresh = false);
        void showEventContextMenu(javelin::gui::calendar::MonthCalendarWidget& widget,
                                  QWidget& popupParent, const QPoint& globalPosition,
                                  const QString& accountId, const QString& eventId,
                                  const QString& recurrenceId);
        void handleEventContextAction(javelin::gui::calendar::MonthCalendarWidget& widget,
                                      const QString& actionId, const QString& accountId,
                                      const QString& eventId, const QString& recurrenceId,
                                      const QString& targetCalendarId);
        void configureEventDialog(javelin::gui::calendar::EventDialog& dialog);
        javelin::gui::settings::GuiSettings& m_settings;
        javelin::app::CalendarReadPort& m_calendarReader;
        javelin::app::CalendarCommandPort& m_calendarCommandPort;
        QStackedWidget& m_contentStack;
        std::vector<TabState>& m_tabs;
        // Account, calendar, and identity reads settle together; a newer read never yields to
        // an older one that completes later.
        AccountSnapshotState m_accountSnapshotState = AccountSnapshotState::Unloaded;
        std::uint64_t m_requestedAccountSnapshot = 0;
        std::uint64_t m_appliedAccountSnapshot = 0;
        std::vector<javelin::jmap::cache::CalendarAccount> m_calendarAccounts;
        std::unordered_map<std::string, std::vector<javelin::jmap::calendar::Calendar>>
            m_accountCalendars;
        std::unordered_map<std::string, std::vector<javelin::jmap::calendar::ParticipantIdentity>>
            m_accountIdentities;
        std::optional<PendingCalendarOpen> m_pendingOpen;
        // The cached range the calendar widget currently presents; menus, dialogs, and agenda
        // views read it so they act on exactly what is displayed.
        std::optional<CalendarViewSnapshot> m_view;
        std::uint64_t m_viewGeneration = 0;
        std::optional<PendingDayAgenda> m_pendingDayAgenda;
        CalendarWorkspaceState m_workspaceState;
        std::function<std::vector<QString>()> m_configuredEventContextMenuLayout;
        std::optional<CalendarEventContextActions> m_eventContextActions;
        javelin::gui::calendar::MonthCalendarWidget* m_eventContextWidget = nullptr;
        QString m_eventContextAccountId;
        QString m_eventContextEventId;
        QString m_eventContextRecurrenceId;
    };
} // namespace javelin::gui::shell
