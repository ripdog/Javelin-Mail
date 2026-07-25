#pragma once

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

namespace javelin::app
{
    class MailApplicationService;
}

namespace javelin::jmap::calendar
{
    class CalendarService;
}

namespace javelin::gui::calendar
{
    class MonthCalendarWidget;
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
        CalendarTabController(javelin::jmap::calendar::CalendarService& calendarService,
                              javelin::app::MailApplicationService& mailService,
                              QStackedWidget& contentStack, std::vector<TabState>& tabs,
                              QObject* parent = nullptr);

        void open(std::optional<QDate> displayedMonth = std::nullopt);
        void invoke(const TabState* tab, CalendarTabCommand command);
        [[nodiscard]] bool refresh(const TabState* tab);
        [[nodiscard]] bool close(TabState& tab);
        [[nodiscard]] QWidget* contentWidgetForTab(const TabState* tab) const;
        [[nodiscard]] QMenu* calendarMenuForTab(const TabState* tab) const;

      Q_SIGNALS:
        void tabReady(int index);
        void statusMessage(QString message, int durationMilliseconds);
        void operationFailed(javelin::jmap::OperationError error);

      private:
        [[nodiscard]] javelin::gui::calendar::MonthCalendarWidget*
        widgetForTab(const TabState* tab) const;

        javelin::jmap::calendar::CalendarService& m_calendarService;
        javelin::app::MailApplicationService& m_mailService;
        QStackedWidget& m_contentStack;
        std::vector<TabState>& m_tabs;
    };
} // namespace javelin::gui::shell
