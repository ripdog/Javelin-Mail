#pragma once

#include "app/CalendarApplicationPorts.h"
#include "gui/shell/TabWorkspace.h"
#include "jmap/OperationError.h"

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
        NextMonth,
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
        void invoke(const TabState* tab, CalendarTabCommand command);
        [[nodiscard]] bool
        available(std::optional<std::string_view> accountId = std::nullopt) const;
        [[nodiscard]] bool refresh(const TabState* tab);
        [[nodiscard]] bool close(TabState& tab);
        [[nodiscard]] QWidget* contentWidgetForTab(const TabState* tab) const;
        [[nodiscard]] QMenu* calendarMenuForTab(const TabState* tab) const;
        void applicationPaletteChanged();
        void
        configureEventContextMenu(QMenu& menu,
                                  std::function<std::vector<QString>()> configuredLayout,
                                  std::function<void(const QList<QAction*>&)> replaceActionList,
                                  CalendarEventContextActions actions);

      Q_SIGNALS:
        void tabReady(int index);
        void statusMessage(QString message, int durationMilliseconds);
        void operationFailed(javelin::jmap::OperationError error);

      private:
        [[nodiscard]] javelin::gui::calendar::MonthCalendarWidget*
        widgetForTab(const TabState* tab) const;
        void showEventContextMenu(javelin::gui::calendar::MonthCalendarWidget& widget,
                                  const QPoint& globalPosition, const QString& accountId,
                                  const QString& eventId, const QString& recurrenceId);
        void handleEventContextAction(javelin::gui::calendar::MonthCalendarWidget& widget,
                                      const QString& actionId, const QString& accountId,
                                      const QString& eventId, const QString& recurrenceId,
                                      const QString& targetCalendarId);

        javelin::gui::settings::GuiSettings& m_settings;
        javelin::jmap::calendar::CalendarReader& m_calendarReader;
        javelin::app::CalendarCommandPort& m_calendarCommandPort;
        QStackedWidget& m_contentStack;
        std::vector<TabState>& m_tabs;
        QMenu* m_eventContextMenu = nullptr;
        std::function<std::vector<QString>()> m_configuredEventContextMenuLayout;
        std::function<void(const QList<QAction*>&)> m_replaceEventContextMenuActionList;
        std::optional<CalendarEventContextActions> m_eventContextActions;
        QList<QObject*> m_eventContextMenuObjects;
        javelin::gui::calendar::MonthCalendarWidget* m_eventContextWidget = nullptr;
        QString m_eventContextAccountId;
        QString m_eventContextEventId;
        QString m_eventContextRecurrenceId;
    };
} // namespace javelin::gui::shell
