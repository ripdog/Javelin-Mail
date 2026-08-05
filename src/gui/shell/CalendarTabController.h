#pragma once

#include "app/CalendarApplicationPorts.h"
#include "gui/shell/TabWorkspace.h"
#include "jmap/OperationError.h"

#include <QDate>
#include <QObject>
#include <QString>

#include <optional>
#include <vector>

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
        [[nodiscard]] bool refresh(const TabState* tab);
        [[nodiscard]] bool close(TabState& tab);
        [[nodiscard]] QWidget* contentWidgetForTab(const TabState* tab) const;
        [[nodiscard]] QMenu* calendarMenuForTab(const TabState* tab) const;
        void applicationPaletteChanged();

      Q_SIGNALS:
        void tabReady(int index);
        void statusMessage(QString message, int durationMilliseconds);
        void operationFailed(javelin::jmap::OperationError error);

      private:
        [[nodiscard]] javelin::gui::calendar::MonthCalendarWidget*
        widgetForTab(const TabState* tab) const;

        javelin::gui::settings::GuiSettings& m_settings;
        javelin::jmap::calendar::CalendarReader& m_calendarReader;
        javelin::app::CalendarCommandPort& m_calendarCommandPort;
        QStackedWidget& m_contentStack;
        std::vector<TabState>& m_tabs;
    };
} // namespace javelin::gui::shell
