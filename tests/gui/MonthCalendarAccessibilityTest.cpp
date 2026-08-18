#include "gui/NoRttiAccessibleObject.h"
#include "gui/calendar/CalendarEventButton.h"
#include "gui/calendar/MonthCalendarWidget.h"
#include "gui/settings/WorkspaceSettingsPort.h"

#include <QAccessible>
#include <QApplication>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QToolButton>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <ranges>

namespace
{
    class TestWorkspaceSettingsPort final : public javelin::gui::settings::WorkspaceSettingsPort
    {
      public:
        [[nodiscard]] const javelin::protocol::WorkspaceSettings& workspaceSettings() const override
        {
            return m_settings;
        }

        [[nodiscard]] std::optional<javelin::protocol::BoundaryError>
        updateWorkspace(javelin::protocol::WorkspaceSettings workspace) override
        {
            m_settings = std::move(workspace);
            return std::nullopt;
        }

        [[nodiscard]] QMetaObject::Connection
        connectWorkspaceChanged(QObject* context, std::function<void()> callback) override
        {
            Q_UNUSED(context);
            Q_UNUSED(callback);
            return {};
        }

      private:
        javelin::protocol::WorkspaceSettings m_settings;
    };

    [[nodiscard]] QAccessibleInterface*
    cellForDate(javelin::gui::calendar::MonthCalendarWidget& widget, const QDate& date)
    {
        auto* accessible = QAccessible::queryAccessibleInterface(&widget);
        if (accessible == nullptr || accessible->tableInterface() == nullptr)
            return nullptr;
        for (int index = 0; index < widget.cellCount(); ++index)
        {
            if (widget.cellDate(index) == date)
                return accessible->tableInterface()->cellAt(index / 7, index % 7);
        }
        return nullptr;
    }
} // namespace

TEST_CASE("calendar accessibility factories ignore Qt objects without C++ RTTI",
          "[gui][calendar][accessibility]")
{
    javelin::gui::calendar::CalendarEventButton button;
    NoRttiAccessibleObjectHandle foreignObject;

    CHECK(QAccessible::queryAccessibleInterface(foreignObject.get()) == nullptr);
}

TEST_CASE("setting the displayed calendar month is idempotent", "[gui][calendar]")
{
    TestWorkspaceSettingsPort settings;
    javelin::gui::calendar::MonthCalendarWidget widget{settings};
    int visibleIntervalChangeCount = 0;
    QObject::connect(&widget, &javelin::gui::calendar::MonthCalendarWidget::visibleIntervalChanged,
                     &widget, [&visibleIntervalChangeCount](const QDate&, const QDate&)
                     { ++visibleIntervalChangeCount; });

    const auto displayedMonth = widget.displayedMonth();
    widget.setDisplayedMonth(displayedMonth);
    CHECK(visibleIntervalChangeCount == 0);

    widget.setDisplayedMonth(displayedMonth.addMonths(1));
    CHECK(visibleIntervalChangeCount == 1);
}

TEST_CASE("month calendar accessibility exposes one named table with concise date cells",
          "[gui][calendar][accessibility]")
{
    TestWorkspaceSettingsPort settings;
    javelin::gui::calendar::MonthCalendarWidget widget{settings};
    widget.setLocale(QLocale{QLocale::English, QLocale::UnitedKingdom});
    widget.setDisplayedMonth(QDate{2026, 3, 1});

    auto* accessible = QAccessible::queryAccessibleInterface(&widget);
    REQUIRE(accessible != nullptr);
    CHECK(accessible->role() == QAccessible::Table);
    CHECK(accessible->text(QAccessible::Name) == QStringLiteral("March 2026"));

    auto* table = accessible->tableInterface();
    REQUIRE(table != nullptr);
    CHECK(table->rowCount() == 6);
    CHECK(table->columnCount() == 7);
    CHECK(table->columnDescription(0) == QStringLiteral("Monday"));

    auto* marchTen = cellForDate(widget, QDate{2026, 3, 10});
    REQUIRE(marchTen != nullptr);
    CHECK(marchTen->role() == QAccessible::Cell);
    CHECK(marchTen->text(QAccessible::Name) == QStringLiteral("Tuesday 10"));
    REQUIRE(marchTen->tableCellInterface() != nullptr);
    CHECK(marchTen->tableCellInterface()->rowIndex() == 2);
    CHECK(marchTen->tableCellInterface()->columnIndex() == 1);
    REQUIRE(marchTen->tableCellInterface()->columnHeaderCells().size() == 1);
    CHECK(marchTen->tableCellInterface()->columnHeaderCells().front()->role() ==
          QAccessible::ColumnHeader);
    CHECK(marchTen->tableCellInterface()->columnHeaderCells().front()->text(QAccessible::Name) ==
          QStringLiteral("Tuesday"));

    auto* februaryTwentyThree = cellForDate(widget, QDate{2026, 2, 23});
    REQUIRE(februaryTwentyThree != nullptr);
    CHECK(februaryTwentyThree->text(QAccessible::Name) == QStringLiteral("Monday 23 February"));
    CHECK_FALSE(februaryTwentyThree->text(QAccessible::Name).contains(QStringLiteral("2026")));

    auto* selection = accessible->selectionInterface();
    REQUIRE(selection != nullptr);
    CHECK(selection->select(marchTen));
    CHECK(selection->selectedItemCount() == 1);
    CHECK(selection->selectedItem(0) == marchTen);
    CHECK(marchTen->state().selected);
}

TEST_CASE("month calendar accessibility reports event counts and full event button names",
          "[gui][calendar][accessibility]")
{
    TestWorkspaceSettingsPort settings;
    javelin::gui::calendar::MonthCalendarWidget widget{settings};
    widget.setLocale(QLocale{QLocale::English, QLocale::UnitedKingdom});
    widget.setDisplayedMonth(QDate{2026, 3, 1});
    widget.resize(640, 400);

    javelin::gui::calendar::MonthEvent event;
    event.accountId = "account";
    event.calendarId = "calendar";
    event.eventId = "event";
    event.title = QStringLiteral("A deliberately long planning meeting title");
    event.start = QDateTime{QDate{2026, 3, 10}, QTime{9, 0}};
    event.end = QDateTime{QDate{2026, 3, 10}, QTime{10, 0}};
    event.recurring = true;
    widget.setEvents({event});

    auto* cell = cellForDate(widget, QDate{2026, 3, 10});
    REQUIRE(cell != nullptr);
    CHECK(cell->text(QAccessible::Name).contains(QStringLiteral("1 event")));
    REQUIRE(cell->childCount() == 1);
    auto* eventButton = cell->child(0);
    REQUIRE(eventButton != nullptr);
    CHECK(dynamic_cast<javelin::gui::calendar::CalendarEventButton*>(eventButton->object()) !=
          nullptr);
    CHECK(eventButton->role() == QAccessible::Button);
    CHECK_FALSE(eventButton->state().checkable);
    REQUIRE(eventButton->actionInterface() != nullptr);
    CHECK(eventButton->actionInterface()->actionNames().contains(
        QAccessibleActionInterface::pressAction()));
    const auto eventName = eventButton->text(QAccessible::Name);
    CHECK(eventName.contains(QStringLiteral("A deliberately long planning meeting title")));
    CHECK(eventName.contains(QStringLiteral("recurring")));
}

TEST_CASE("month calendar pending invitation banner sorts and activates invitations",
          "[gui][calendar][invitation][accessibility]")
{
    TestWorkspaceSettingsPort settings;
    javelin::gui::calendar::MonthCalendarWidget widget{settings};
    widget.setLocale(QLocale{QLocale::English, QLocale::NewZealand});
    widget.setDisplayedMonth(QDate{2026, 8, 1});
    widget.setPendingInvitations({
        {.accountId = QStringLiteral("a1"),
         .eventId = QStringLiteral("later"),
         .recurrenceId = {},
         .navigationDate = QDate{2026, 8, 22},
         .title = QStringLiteral("Later planning"),
         .organizer = QStringLiteral("Bob"),
         .displayTime = QDateTime{QDate{2026, 8, 22}, QTime{14, 0}},
         .allDay = false},
        {.accountId = QStringLiteral("a2"),
         .eventId = QStringLiteral("earlier"),
         .recurrenceId = QStringLiteral("2026-08-20T09:00:00"),
         .navigationDate = QDate{2026, 8, 20},
         .title = QStringLiteral("Earlier planning"),
         .organizer = QStringLiteral("Alice"),
         .displayTime = QDateTime{QDate{2026, 8, 20}, QTime{9, 0}},
         .allDay = false},
    });
    widget.show();
    QApplication::processEvents();

    auto* banner = widget.findChild<QWidget*>(QStringLiteral("calendarInvitationBanner"));
    auto* label = widget.findChild<QLabel*>(QStringLiteral("calendarInvitationBannerLabel"));
    auto* view = widget.findChild<QToolButton*>(QStringLiteral("calendarViewInvitations"));
    auto* menu = widget.findChild<QMenu*>(QStringLiteral("calendarInvitationMenu"));
    REQUIRE(banner != nullptr);
    REQUIRE(label != nullptr);
    REQUIRE(view != nullptr);
    REQUIRE(menu != nullptr);
    CHECK(banner->isVisible());
    CHECK(label->text().contains(QStringLiteral("2 invitations awaiting response")));
    CHECK(view->accessibleName().contains(QStringLiteral("pending calendar invitations")));
    REQUIRE(menu->actions().size() == 2);
    CHECK(menu->actions()[0]->text().startsWith(QStringLiteral("Earlier planning")));
    CHECK(menu->actions()[0]->text().contains(QStringLiteral("Alice")));
    CHECK(menu->actions()[1]->text().startsWith(QStringLiteral("Later planning")));

    QString activatedAccount;
    QString activatedEvent;
    QString activatedRecurrence;
    QDate activatedDate;
    QObject::connect(
        &widget, &javelin::gui::calendar::MonthCalendarWidget::pendingInvitationActivated, &widget,
        [&](const QString& accountId, const QString& eventId, const QString& recurrenceId,
            const QDate& navigationDate)
        {
            activatedAccount = accountId;
            activatedEvent = eventId;
            activatedRecurrence = recurrenceId;
            activatedDate = navigationDate;
        });
    menu->actions()[0]->trigger();
    CHECK(activatedAccount == QStringLiteral("a2"));
    CHECK(activatedEvent == QStringLiteral("earlier"));
    CHECK(activatedRecurrence == QStringLiteral("2026-08-20T09:00:00"));
    CHECK(activatedDate == QDate{2026, 8, 20});

    widget.setPendingInvitations({});
    QApplication::processEvents();
    CHECK_FALSE(banner->isVisible());
}

TEST_CASE("month calendar exposes the materialized events for a requested day", "[gui][calendar]")
{
    TestWorkspaceSettingsPort settings;
    javelin::gui::calendar::MonthCalendarWidget widget{settings};
    widget.setDisplayedMonth(QDate{2026, 8, 1});

    javelin::gui::calendar::MonthEvent event;
    event.accountId = "account";
    event.calendarId = "account\ncalendar";
    event.eventId = "event";
    event.title = QStringLiteral("Overnight event");
    event.start = QDateTime{QDate{2026, 8, 9}, QTime{23, 0}};
    event.end = QDateTime{QDate{2026, 8, 10}, QTime{1, 0}};
    widget.setEvents({event});

    const auto augustNine = widget.eventsForDate(QDate{2026, 8, 9});
    const auto augustTen = widget.eventsForDate(QDate{2026, 8, 10});
    const auto augustEleven = widget.eventsForDate(QDate{2026, 8, 11});
    REQUIRE(augustNine.size() == 1);
    REQUIRE(augustTen.size() == 1);
    CHECK(augustNine.front().eventId == "event");
    CHECK(augustTen.front().eventId == "event");
    CHECK(augustEleven.empty());
}

TEST_CASE("month calendar event activation requests the event's actual day", "[gui][calendar]")
{
    TestWorkspaceSettingsPort settings;
    javelin::gui::calendar::MonthCalendarWidget widget{settings};
    widget.setDisplayedMonth(QDate{2026, 8, 1});
    widget.setSelectedDateFromAgenda(QDate{2026, 8, 19});
    widget.resize(900, 700);

    javelin::gui::calendar::MonthEvent event;
    event.accountId = "account";
    event.calendarId = "account\ncalendar";
    event.eventId = "event";
    event.title = QStringLiteral("Water Softener Running");
    event.start = QDateTime{QDate{2026, 8, 12}, QTime{2, 20}};
    event.end = QDateTime{QDate{2026, 8, 12}, QTime{4, 30}};
    widget.setEvents({event});
    widget.show();
    QApplication::processEvents();

    QDate agendaDate;
    QString agendaEventId;
    QString contextEventId;
    QPoint contextPosition;
    QObject::connect(&widget, &javelin::gui::calendar::MonthCalendarWidget::dayAgendaRequested,
                     &widget,
                     [&agendaDate, &agendaEventId](const QDate& date, const QString&,
                                                   const QString& eventId, const QString&)
                     {
                         agendaDate = date;
                         agendaEventId = eventId;
                     });
    QObject::connect(
        &widget, &javelin::gui::calendar::MonthCalendarWidget::eventContextMenuRequested, &widget,
        [&contextEventId, &contextPosition](const QPoint& position, const QString&,
                                            const QString& eventId, const QString&)
        {
            contextPosition = position;
            contextEventId = eventId;
        });

    const auto buttons = widget.findChildren<QToolButton*>();
    const auto eventButton = std::ranges::find_if(
        buttons, [](QToolButton* button)
        { return dynamic_cast<javelin::gui::calendar::CalendarEventButton*>(button) != nullptr; });
    REQUIRE(eventButton != buttons.end());
    QPointer<QToolButton> activatedButton{*eventButton};
    const auto localPosition = QPointF{(*eventButton)->rect().center()};
    const auto globalPosition = QPointF{(*eventButton)->mapToGlobal(localPosition.toPoint())};
    QMouseEvent press{QEvent::MouseButtonPress, localPosition,  globalPosition,
                      Qt::LeftButton,           Qt::LeftButton, Qt::NoModifier};
    QMouseEvent release{QEvent::MouseButtonRelease,
                        localPosition,
                        globalPosition,
                        Qt::LeftButton,
                        Qt::NoButton,
                        Qt::NoModifier};
    QApplication::sendEvent(*eventButton, &press);
    QApplication::sendEvent(*eventButton, &release);
    const QPoint expectedContextPosition{947, 613};
    QContextMenuEvent contextEvent{QContextMenuEvent::Mouse, QPoint{2, 3}, expectedContextPosition};
    QApplication::sendEvent(*eventButton, &contextEvent);

    CHECK_FALSE(activatedButton.isNull());
    CHECK(agendaDate == QDate{2026, 8, 12});
    CHECK(agendaEventId == QStringLiteral("event"));
    CHECK(contextEventId == QStringLiteral("event"));
    CHECK(contextPosition == expectedContextPosition);
    CHECK(widget.selectedDate() == QDate{2026, 8, 12});
}

TEST_CASE("month calendar page navigation keeps the selected cell in the displayed month",
          "[gui][calendar][accessibility][keyboard]")
{
    TestWorkspaceSettingsPort settings;
    javelin::gui::calendar::MonthCalendarWidget widget{settings};
    widget.setLocale(QLocale{QLocale::English, QLocale::UnitedKingdom});
    widget.setDisplayedMonth(QDate{2026, 1, 1});

    auto* accessible = QAccessible::queryAccessibleInterface(&widget);
    REQUIRE(accessible != nullptr);
    auto* selection = accessible->selectionInterface();
    REQUIRE(selection != nullptr);
    auto* januaryThirtyOne = cellForDate(widget, QDate{2026, 1, 31});
    REQUIRE(januaryThirtyOne != nullptr);
    REQUIRE(selection->select(januaryThirtyOne));
    CHECK(widget.selectedDate() == QDate{2026, 1, 31});

    QKeyEvent pageDown{QEvent::KeyPress, Qt::Key_PageDown, Qt::NoModifier};
    QApplication::sendEvent(&widget, &pageDown);

    CHECK(widget.displayedMonth() == QDate{2026, 2, 1});
    CHECK(widget.selectedDate() == QDate{2026, 2, 28});
    CHECK(accessible->text(QAccessible::Name) == QStringLiteral("February 2026"));
    REQUIRE(selection->selectedItemCount() == 1);
    CHECK(selection->selectedItem(0)->text(QAccessible::Name) == QStringLiteral("Saturday 28"));
}

TEST_CASE("month calendar Enter opens the selected day instead of creating immediately",
          "[gui][calendar][accessibility][keyboard]")
{
    TestWorkspaceSettingsPort settings;
    javelin::gui::calendar::MonthCalendarWidget widget{settings};
    widget.setDisplayedMonth(QDate{2026, 8, 1});

    QDate agendaDate;
    int createRequests = 0;
    QObject::connect(
        &widget, &javelin::gui::calendar::MonthCalendarWidget::dayAgendaRequested, &widget,
        [&agendaDate](const QDate& date, const QString&, const QString&, const QString&)
        { agendaDate = date; });
    QObject::connect(&widget, &javelin::gui::calendar::MonthCalendarWidget::emptyTimeActivated,
                     &widget,
                     [&createRequests](const QDateTime&, const QDateTime&) { ++createRequests; });

    QKeyEvent enter{QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QApplication::sendEvent(&widget, &enter);

    CHECK(agendaDate == widget.selectedDate());
    CHECK(createRequests == 0);
}
