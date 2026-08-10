#include "gui/calendar/DayAgendaDialog.h"
#include "gui/calendar/CalendarEventButton.h"

#include <QAccessible>
#include <QApplication>
#include <QFontMetrics>
#include <QImage>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolButton>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace
{
    [[nodiscard]] javelin::gui::calendar::DayAgendaEvent event(const QString& id,
                                                               const QTime& start, const QTime& end,
                                                               const QString& title,
                                                               const bool editable = true)
    {
        return {
            .key = {.accountId = QStringLiteral("account"), .eventId = id, .recurrenceId = {}},
            .title = title,
            .calendarName = QStringLiteral("Personal"),
            .color = QColor{QStringLiteral("#336699")},
            .start = QDateTime{QDate{2026, 8, 10}, start},
            .end = QDateTime{QDate{2026, 8, 10}, end},
            .allDay = false,
            .recurring = false,
            .editable = editable,
            .invitation = false,
            .organizer = QStringLiteral("Alice <alice@example.test>"),
            .location = QStringLiteral("Meeting room"),
            .description = QStringLiteral("Detailed agenda text"),
            .attendees = {QStringLiteral("Bob <bob@example.test>")},
        };
    }

    void settleGui()
    {
        QApplication::processEvents();
        QApplication::processEvents();
    }
} // namespace

TEST_CASE("day agenda renders a midnight-to-midnight timeline and starts at eight",
          "[gui][calendar][agenda]")
{
    javelin::gui::calendar::DayAgendaDialog dialog;
    dialog.setDay(QDate{2026, 8, 10}, {event(QStringLiteral("morning"), QTime{9, 0}, QTime{10, 0},
                                             QStringLiteral("Morning meeting"))});
    dialog.show();
    settleGui();

    CHECK(dialog.windowTitle().contains(QStringLiteral("10 August 2026")));
    auto* timeline = dialog.findChild<QWidget*>(QStringLiteral("dayAgendaTimeline"));
    REQUIRE(timeline != nullptr);
    CHECK(timeline->minimumHeight() == 24 * 64 + 1);
    QImage image{timeline->size(), QImage::Format_ARGB32_Premultiplied};
    image.fill(Qt::transparent);
    timeline->render(&image);
    CHECK(image.pixelColor(100, 15 * 64 / 60).rgba() ==
          timeline->palette().color(QPalette::Midlight).rgba());
    CHECK(image.pixelColor(100, 60 * 64 / 60).rgba() ==
          timeline->palette().color(QPalette::Mid).rgba());

    auto* scroll = dialog.findChild<QScrollArea*>(QStringLiteral("dayAgendaTimelineScroll"));
    REQUIRE(scroll != nullptr);
    CHECK(scroll->verticalScrollBar()->value() == 8 * 64);

    const auto buttons = dialog.findChildren<QToolButton*>(QStringLiteral("dayAgendaEventButton"));
    REQUIRE(buttons.size() == 1);
    CHECK(buttons.front()->y() >= 9 * 64);
    CHECK(buttons.front()->accessibleName().contains(QStringLiteral("Morning meeting")));
    dialog.close();
}

TEST_CASE("day agenda time gutter expands for the active font", "[gui][calendar][agenda]")
{
    javelin::gui::calendar::DayAgendaDialog dialog;
    auto font = dialog.font();
    font.setPointSize(28);
    dialog.setFont(font);
    dialog.setDay(QDate{2026, 8, 10}, {event(QStringLiteral("morning"), QTime{9, 0}, QTime{10, 0},
                                             QStringLiteral("Morning meeting"))});
    dialog.show();
    settleGui();

    auto* timeline = dialog.findChild<QWidget*>(QStringLiteral("dayAgendaTimeline"));
    REQUIRE(timeline != nullptr);
    const auto buttons = dialog.findChildren<QToolButton*>(QStringLiteral("dayAgendaEventButton"));
    REQUIRE(buttons.size() == 1);

    const QFontMetrics metrics{timeline->font()};
    int widestHour = 0;
    for (int hour = 0; hour < 24; ++hour)
    {
        widestHour = std::max(widestHour, metrics.horizontalAdvance(QLocale{}.toString(
                                              QTime{hour, 0}, QLocale::ShortFormat)));
    }
    CHECK(buttons.front()->x() > widestHour);
    dialog.close();
}

TEST_CASE("day agenda scrolls away from eight when needed to reveal the selected event",
          "[gui][calendar][agenda]")
{
    javelin::gui::calendar::DayAgendaDialog dialog;
    const auto early =
        event(QStringLiteral("early"), QTime{2, 0}, QTime{3, 0}, QStringLiteral("Early event"));
    dialog.setDay(QDate{2026, 8, 10}, {early}, early.key);
    dialog.show();
    settleGui();

    auto* scroll = dialog.findChild<QScrollArea*>(QStringLiteral("dayAgendaTimelineScroll"));
    REQUIRE(scroll != nullptr);
    CHECK(scroll->verticalScrollBar()->value() < 8 * 64);
    const auto buttons = dialog.findChildren<QToolButton*>(QStringLiteral("dayAgendaEventButton"));
    REQUIRE(buttons.size() == 1);
    CHECK(buttons.front()->hasFocus());
    dialog.close();
}

TEST_CASE("day agenda lays overlapping timed events side by side", "[gui][calendar][agenda]")
{
    javelin::gui::calendar::DayAgendaDialog dialog;
    dialog.setDay(
        QDate{2026, 8, 10},
        {event(QStringLiteral("left"), QTime{9, 0}, QTime{10, 0}, QStringLiteral("First")),
         event(QStringLiteral("right"), QTime{9, 30}, QTime{10, 30}, QStringLiteral("Second"))});
    dialog.show();
    settleGui();

    const auto buttons = dialog.findChildren<QToolButton*>(QStringLiteral("dayAgendaEventButton"));
    REQUIRE(buttons.size() == 2);
    CHECK_FALSE(buttons[0]->geometry().intersects(buttons[1]->geometry()));
    auto* timeline = dialog.findChild<QWidget*>(QStringLiteral("dayAgendaTimeline"));
    REQUIRE(timeline != nullptr);
    CHECK(buttons[0]->width() < timeline->width());
    CHECK(buttons[1]->width() < timeline->width());
    dialog.close();
}

TEST_CASE("day agenda event selection fills details and exposes edit explicitly",
          "[gui][calendar][agenda]")
{
    javelin::gui::calendar::DayAgendaDialog dialog;
    const auto selected = event(QStringLiteral("selected"), QTime{11, 0}, QTime{12, 0},
                                QStringLiteral("Planning session"));
    dialog.setDay(QDate{2026, 8, 10}, {selected});
    dialog.show();
    settleGui();

    const auto buttons = dialog.findChildren<QToolButton*>(QStringLiteral("dayAgendaEventButton"));
    REQUIRE(buttons.size() == 1);
    buttons.front()->click();
    settleGui();

    auto* title = dialog.findChild<QLabel*>(QStringLiteral("dayAgendaDetailsTitle"));
    REQUIRE(title != nullptr);
    CHECK(title->text() == QStringLiteral("Planning session"));
    auto* edit = dialog.findChild<QPushButton*>(QStringLiteral("dayAgendaEditButton"));
    REQUIRE(edit != nullptr);
    CHECK(edit->isVisible());
    CHECK(edit->isEnabled());

    bool editRequested = false;
    QObject::connect(&dialog, &javelin::gui::calendar::DayAgendaDialog::editRequested, &dialog,
                     [&editRequested](const QString&, const QString&, const QString&)
                     { editRequested = true; });
    edit->click();
    CHECK(editRequested);
    dialog.close();
}

TEST_CASE("day agenda accessibility exposes schedule events and selected details relations",
          "[gui][calendar][agenda][accessibility]")
{
    javelin::gui::calendar::DayAgendaDialog dialog;
    const auto first =
        event(QStringLiteral("first"), QTime{9, 0}, QTime{10, 0}, QStringLiteral("First meeting"));
    const auto second = event(QStringLiteral("second"), QTime{11, 0}, QTime{12, 0},
                              QStringLiteral("Second meeting"));
    dialog.setDay(QDate{2026, 8, 10}, {first, second});
    dialog.show();
    settleGui();

    auto* timeline = dialog.findChild<QWidget*>(QStringLiteral("dayAgendaTimeline"));
    REQUIRE(timeline != nullptr);
    auto* timelineAccessible = QAccessible::queryAccessibleInterface(timeline);
    REQUIRE(timelineAccessible != nullptr);
    CHECK(timelineAccessible->role() == QAccessible::Pane);
    CHECK(timelineAccessible->text(QAccessible::Name).contains(QStringLiteral("10 August 2026")));
    CHECK(timelineAccessible->text(QAccessible::Description).contains(QStringLiteral("2 timed")));
    CHECK(timelineAccessible->childCount() == 2);

    auto* details = dialog.findChild<QScrollArea*>(QStringLiteral("dayAgendaDetailsPane"));
    REQUIRE(details != nullptr);
    auto* detailsAccessible = QAccessible::queryAccessibleInterface(details);
    REQUIRE(detailsAccessible != nullptr);
    CHECK(detailsAccessible->role() == QAccessible::Pane);
    CHECK(detailsAccessible->text(QAccessible::Name) == QStringLiteral("Event details"));
    CHECK(detailsAccessible->text(QAccessible::Description) == QStringLiteral("No event selected"));
    CHECK(detailsAccessible->relations(QAccessible::Controlled).isEmpty());

    const auto buttons = dialog.findChildren<QToolButton*>(QStringLiteral("dayAgendaEventButton"));
    REQUIRE(buttons.size() == 2);
    auto* firstButton = dynamic_cast<javelin::gui::calendar::CalendarEventButton*>(buttons[0]);
    auto* secondButton = dynamic_cast<javelin::gui::calendar::CalendarEventButton*>(buttons[1]);
    REQUIRE(firstButton != nullptr);
    REQUIRE(secondButton != nullptr);

    auto* firstAccessible = QAccessible::queryAccessibleInterface(firstButton);
    REQUIRE(firstAccessible != nullptr);
    CHECK(firstAccessible->role() == QAccessible::Button);
    REQUIRE(firstAccessible->actionInterface() != nullptr);
    CHECK(firstAccessible->actionInterface()->actionNames().contains(
        QAccessibleActionInterface::pressAction()));
    const auto controlled = firstAccessible->relations(QAccessible::Controller);
    REQUIRE(controlled.size() == 1);
    CHECK(controlled.front().first->object() == details);
    CHECK(controlled.front().second == QAccessible::Controller);

    firstButton->click();
    settleGui();
    CHECK(firstAccessible->state().checkable);
    CHECK(firstAccessible->state().checked);
    CHECK(detailsAccessible->text(QAccessible::Description)
              .contains(QStringLiteral("First meeting")));
    CHECK(
        detailsAccessible->text(QAccessible::Description).contains(QStringLiteral("Meeting room")));
    auto selectedController = detailsAccessible->relations(QAccessible::Controlled);
    REQUIRE(selectedController.size() == 1);
    CHECK(selectedController.front().first->object() == firstButton);
    CHECK(selectedController.front().second == QAccessible::Controlled);

    secondButton->click();
    settleGui();
    CHECK_FALSE(firstAccessible->state().checked);
    CHECK(detailsAccessible->text(QAccessible::Description)
              .contains(QStringLiteral("Second meeting")));
    selectedController = detailsAccessible->relations(QAccessible::Controlled);
    REQUIRE(selectedController.size() == 1);
    CHECK(selectedController.front().first->object() == secondButton);
    dialog.close();
}

TEST_CASE("day agenda navigation changes its day and leaves creation explicit",
          "[gui][calendar][agenda][keyboard]")
{
    javelin::gui::calendar::DayAgendaDialog dialog;
    dialog.setDay(QDate{2026, 8, 10}, {});
    dialog.show();
    settleGui();

    QDate requestedDay;
    QDate createDay;
    QObject::connect(&dialog, &javelin::gui::calendar::DayAgendaDialog::dayChanged, &dialog,
                     [&requestedDay](const QDate& date) { requestedDay = date; });
    QObject::connect(&dialog, &javelin::gui::calendar::DayAgendaDialog::newEventRequested, &dialog,
                     [&createDay](const QDate& date) { createDay = date; });

    QToolButton* next = nullptr;
    for (auto* candidate : dialog.findChildren<QToolButton*>())
    {
        if (candidate->accessibleName() == QStringLiteral("Next day"))
        {
            next = candidate;
            break;
        }
    }
    REQUIRE(next != nullptr);
    next->click();
    CHECK(requestedDay == QDate{2026, 8, 11});
    CHECK(dialog.date() == QDate{2026, 8, 11});
    CHECK(dialog.windowTitle().contains(QStringLiteral("11 August 2026")));

    QPushButton* create = nullptr;
    for (auto* candidate : dialog.findChildren<QPushButton*>())
    {
        if (candidate->text() == QStringLiteral("New Event"))
        {
            create = candidate;
            break;
        }
    }
    REQUIRE(create != nullptr);
    create->click();
    CHECK(createDay == QDate{2026, 8, 11});
    dialog.close();
}
