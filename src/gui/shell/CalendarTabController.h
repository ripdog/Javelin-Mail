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
#include <QString>

#include <functional>
#include <optional>
#include <string_view>
#include <vector>

class QAction;
class QMenu;
class QStackedWidget;
class QWidget;

namespace javelin::jmap::calendar
{
    class CalendarReader;
}

namespace javelin::gui::calendar
{
    class MonthCalendarWidget;
}

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

    class CalendarTabController final : public QObject
    {
        Q_OBJECT

      public:
        CalendarTabController(javelin::gui::settings::GuiSettings& settings,
                              javelin::jmap::calendar::CalendarReader& calendarReader,
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
        [[nodiscard]] CalendarWorkspaceState calculateWorkspaceState() const;
        void refreshWorkspaceState();
        [[nodiscard]] javelin::gui::calendar::MonthCalendarWidget*
        widgetForTab(const TabState* tab) const;
        [[nodiscard]] bool
        refreshAccountSnapshot(javelin::gui::calendar::MonthCalendarWidget& widget);
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
        javelin::gui::settings::GuiSettings& m_settings;
        javelin::jmap::calendar::CalendarReader& m_calendarReader;
        javelin::app::CalendarCommandPort& m_calendarCommandPort;
        QStackedWidget& m_contentStack;
        std::vector<TabState>& m_tabs;
        std::vector<javelin::jmap::cache::CalendarAccount> m_calendarAccounts;
        CalendarWorkspaceState m_workspaceState;
        std::function<std::vector<QString>()> m_configuredEventContextMenuLayout;
        std::optional<CalendarEventContextActions> m_eventContextActions;
        javelin::gui::calendar::MonthCalendarWidget* m_eventContextWidget = nullptr;
        QString m_eventContextAccountId;
        QString m_eventContextEventId;
        QString m_eventContextRecurrenceId;
    };
} // namespace javelin::gui::shell
