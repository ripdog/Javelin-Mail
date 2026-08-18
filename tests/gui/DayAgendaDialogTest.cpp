#include "gui/calendar/DayAgendaDialog.h"
#include "gui/calendar/CalendarEventButton.h"
#include "gui/calendar/CalendarPresentation.h"
#include "gui/calendar/MonthCalendarWidget.h"

#include <QAccessible>
#include <QApplication>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QImage>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
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
            .rsvpAllowed = false,
            .participationStatus = {},
            .responseMutationPending = false,
            .responseError = {},
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

    void sendMouseEvent(QWidget* widget, const QEvent::Type type, const QPointF& position,
                        const Qt::MouseButton button, const Qt::MouseButtons buttons)
    {
        QMouseEvent event{type,   position, widget->mapToGlobal(position.toPoint()),
                          button, buttons,  Qt::NoModifier};
        QApplication::sendEvent(widget, &event);
    }
} // namespace

TEST_CASE("day agenda base events preserve the month presentation without detail data",
          "[gui][calendar][agenda][presentation]")
{
    const javelin::gui::calendar::MonthEvent monthEvent{
        .accountId = "account",
        .calendarId = "account\\ncalendar",
        .eventId = "event",
        .title = QStringLiteral("Shared presentation"),
        .color = QColor{QStringLiteral("#5a7fb2")},
        .start = QDateTime{QDate{2026, 8, 10}, QTime{9, 15}},
        .end = QDateTime{QDate{2026, 8, 10}, QTime{10, 45}},
        .allDay = false,
        .recurrenceId = std::optional<std::string>{"2026-08-10T09:15:00"},
        .recurring = true,
    };

    const auto agendaEvent = javelin::gui::calendar::dayAgendaEventFromMonthEvent(monthEvent);
    CHECK(agendaEvent.key.accountId == QStringLiteral("account"));
    CHECK(agendaEvent.key.eventId == QStringLiteral("event"));
    CHECK(agendaEvent.key.recurrenceId == QStringLiteral("2026-08-10T09:15:00"));
    CHECK(agendaEvent.title == monthEvent.title);
    CHECK(agendaEvent.color == monthEvent.color);
    CHECK(agendaEvent.start == monthEvent.start);
    CHECK(agendaEvent.end == monthEvent.end);
    CHECK(agendaEvent.allDay == monthEvent.allDay);
    CHECK(agendaEvent.recurring == monthEvent.recurring);
    CHECK_FALSE(agendaEvent.editable);
    CHECK_FALSE(agendaEvent.rsvpAllowed);
}

TEST_CASE("day agenda renders a midnight-to-midnight timeline and starts at eight",
          "[gui][calendar][agenda]")
{
    javelin::gui::calendar::DayAgendaDialog dialog;
    auto morning = event(QStringLiteral("morning"), QTime{9, 0}, QTime{10, 0},
                         QStringLiteral("Morning meeting"));
    morning.recurring = true;
    dialog.setDay(QDate{2026, 8, 10}, {morning});
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
    auto monthPresentation = javelin::gui::calendar::CalendarEventButton{};
    monthPresentation.setCalendarEventPresentation(
        QStringLiteral("Morning meeting"), QDateTime{QDate{2026, 8, 10}, QTime{9, 0}},
        QDateTime{QDate{2026, 8, 10}, QTime{10, 0}}, false, true, QDate{2026, 8, 10},
        QStringLiteral("Morning meeting"), QColor{QStringLiteral("#336699")});
    CHECK(buttons.front()->styleSheet() == monthPresentation.styleSheet());
    CHECK(buttons.front()->sizePolicy() == monthPresentation.sizePolicy());
    CHECK(buttons.front()->text().contains(QStringLiteral("09:00")));
    CHECK(buttons.front()->text().contains(QStringLiteral("Morning meeting")));
    CHECK(buttons.front()->text().contains(QStringLiteral("↻")));
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

TEST_CASE("day agenda RSVP controls preserve confirmed state and surface failures",
          "[gui][calendar][agenda][invitation]")
{
    javelin::gui::calendar::DayAgendaDialog dialog;
    auto selected = event(QStringLiteral("invitation"), QTime{11, 0}, QTime{12, 0},
                          QStringLiteral("Planning invitation"), false);
    selected.rsvpAllowed = true;
    selected.participationStatus = QStringLiteral("tentative");
    selected.recurring = true;
    dialog.setDay(QDate{2026, 8, 10}, {selected}, selected.key);
    dialog.show();
    settleGui();

    auto* accept = dialog.findChild<QPushButton*>(QStringLiteral("dayAgendaRsvpAccept"));
    auto* tentative = dialog.findChild<QPushButton*>(QStringLiteral("dayAgendaRsvpTentative"));
    auto* decline = dialog.findChild<QPushButton*>(QStringLiteral("dayAgendaRsvpDecline"));
    auto* changeResponse = dialog.findChild<QCheckBox*>(QStringLiteral("dayAgendaChangeResponse"));
    auto* responseLabel = dialog.findChild<QLabel*>(QStringLiteral("dayAgendaResponseLabel"));
    REQUIRE(accept != nullptr);
    REQUIRE(tentative != nullptr);
    REQUIRE(decline != nullptr);
    REQUIRE(changeResponse != nullptr);
    REQUIRE(responseLabel != nullptr);
    CHECK_FALSE(accept->isVisible());
    CHECK_FALSE(tentative->isVisible());
    CHECK_FALSE(decline->isVisible());
    CHECK(changeResponse->isVisible());
    CHECK_FALSE(changeResponse->isChecked());
    CHECK(responseLabel->text() == QStringLiteral("Your response: tentative"));

    changeResponse->click();
    settleGui();
    CHECK(changeResponse->isChecked());
    CHECK(accept->isVisible());
    CHECK(tentative->isVisible());
    CHECK(decline->isVisible());
    CHECK(tentative->isChecked());
    CHECK_FALSE(accept->isChecked());
    CHECK_FALSE(decline->isChecked());
    CHECK(accept->isEnabled());

    QString requestedStatus;
    QString requestedRecurrenceId;
    QObject::connect(&dialog, &javelin::gui::calendar::DayAgendaDialog::responseRequested, &dialog,
                     [&requestedStatus, &requestedRecurrenceId](const QString&, const QString&,
                                                                const QString& recurrenceId,
                                                                const QString& status)
                     {
                         requestedRecurrenceId = recurrenceId;
                         requestedStatus = status;
                     });
    accept->click();
    settleGui();
    CHECK(requestedStatus == QStringLiteral("accepted"));
    CHECK(requestedRecurrenceId.isEmpty());
    CHECK_FALSE(accept->isEnabled());
    CHECK_FALSE(tentative->isEnabled());
    CHECK_FALSE(decline->isEnabled());

    dialog.setResponseMutationPending(false, QStringLiteral("Server rejected the response."));
    settleGui();
    CHECK(accept->isEnabled());
    CHECK(tentative->isChecked());
    CHECK_FALSE(accept->isChecked());
    auto* error = dialog.findChild<QLabel*>(QStringLiteral("dayAgendaResponseError"));
    REQUIRE(error != nullptr);
    CHECK(error->isVisible());
    CHECK(error->text() == QStringLiteral("Server rejected the response."));
    CHECK(error->textInteractionFlags().testFlag(Qt::TextSelectableByKeyboard));
    CHECK(changeResponse->isChecked());

    selected.participationStatus = QStringLiteral("accepted");
    dialog.setDay(QDate{2026, 8, 10}, {selected}, selected.key);
    settleGui();
    CHECK_FALSE(changeResponse->isChecked());
    CHECK(changeResponse->isVisible());
    CHECK_FALSE(accept->isVisible());
    CHECK(responseLabel->text() == QStringLiteral("Your response: accepted"));

    const auto labels = dialog.findChildren<QLabel*>();
    CHECK(std::ranges::any_of(labels, [](const QLabel* label)
                              { return label->text().contains(QStringLiteral("entire series")); }));
    dialog.close();
}

TEST_CASE("day agenda occurrence RSVP stays scoped to the selected instance",
          "[gui][calendar][agenda][invitation][recurrence]")
{
    javelin::gui::calendar::DayAgendaDialog dialog;
    auto selected = event(QStringLiteral("recurring-invitation"), QTime{11, 0}, QTime{12, 0},
                          QStringLiteral("Occurrence invitation"), false);
    selected.recurring = true;
    selected.rsvpAllowed = true;
    selected.rsvpRecurrenceId = QStringLiteral("2026-08-10T11:00:00");
    selected.participationStatus = QStringLiteral("needs-action");
    dialog.setDay(QDate{2026, 8, 10}, {selected}, selected.key);
    dialog.show();
    settleGui();

    const auto labels = dialog.findChildren<QLabel*>();
    CHECK(std::ranges::any_of(
        labels, [](const QLabel* label)
        { return label->text().contains(QStringLiteral("only to this occurrence")); }));

    auto* accept = dialog.findChild<QPushButton*>(QStringLiteral("dayAgendaRsvpAccept"));
    REQUIRE(accept != nullptr);
    QString requestedRecurrenceId;
    QString requestedStatus;
    QObject::connect(&dialog, &javelin::gui::calendar::DayAgendaDialog::responseRequested, &dialog,
                     [&requestedRecurrenceId, &requestedStatus](const QString&, const QString&,
                                                                const QString& recurrenceId,
                                                                const QString& status)
                     {
                         requestedRecurrenceId = recurrenceId;
                         requestedStatus = status;
                     });
    accept->click();
    settleGui();
    CHECK(requestedRecurrenceId == QStringLiteral("2026-08-10T11:00:00"));
    CHECK(requestedStatus == QStringLiteral("accepted"));
    dialog.close();
}

TEST_CASE("day agenda keeps remaining events when a selected event disappears",
          "[gui][calendar][agenda][mutation]")
{
    javelin::gui::calendar::DayAgendaDialog dialog;
    const auto removed = event(QStringLiteral("removed"), QTime{9, 0}, QTime{10, 0},
                               QStringLiteral("Removed event"));
    const auto retained = event(QStringLiteral("retained"), QTime{11, 0}, QTime{12, 0},
                                QStringLiteral("Retained event"));
    dialog.setDay(QDate{2026, 8, 10}, {removed, retained}, removed.key);
    dialog.show();
    settleGui();

    dialog.setDay(QDate{2026, 8, 10}, {retained}, removed.key);
    settleGui();

    const auto buttons = dialog.findChildren<QToolButton*>(QStringLiteral("dayAgendaEventButton"));
    REQUIRE(buttons.size() == 1);
    CHECK(buttons.front()->isVisible());
    CHECK(buttons.front()->property("agendaEventId").toString() == QStringLiteral("retained"));
    CHECK(buttons.front()->accessibleName().contains(QStringLiteral("Retained event")));
    CHECK_FALSE(dialog.selectedEvent().has_value());
    auto* title = dialog.findChild<QLabel*>(QStringLiteral("dayAgendaDetailsTitle"));
    REQUIRE(title != nullptr);
    CHECK(title->text() == QStringLiteral("Select an event to see details"));
    dialog.close();
}

TEST_CASE("day agenda event buttons request the shared context menu", "[gui][calendar][agenda]")
{
    javelin::gui::calendar::DayAgendaDialog dialog;
    const auto selected = event(QStringLiteral("context-event"), QTime{11, 0}, QTime{12, 0},
                                QStringLiteral("Context event"));
    dialog.setDay(QDate{2026, 8, 10}, {selected});
    dialog.show();
    settleGui();

    const auto buttons = dialog.findChildren<QToolButton*>(QStringLiteral("dayAgendaEventButton"));
    REQUIRE(buttons.size() == 1);
    QString requestedEventId;
    QPoint requestedPosition;
    QObject::connect(&dialog, &javelin::gui::calendar::DayAgendaDialog::eventContextMenuRequested,
                     &dialog,
                     [&requestedEventId, &requestedPosition](const QPoint& position, const QString&,
                                                             const QString& eventId, const QString&)
                     {
                         requestedPosition = position;
                         requestedEventId = eventId;
                     });
    auto* eventButton = dynamic_cast<javelin::gui::calendar::CalendarEventButton*>(buttons.front());
    REQUIRE(eventButton != nullptr);
    const QPoint expectedGlobal{731, 419};
    QContextMenuEvent contextEvent{QContextMenuEvent::Mouse, QPoint{3, 4}, expectedGlobal};
    QApplication::sendEvent(eventButton, &contextEvent);
    CHECK(requestedEventId == QStringLiteral("context-event"));
    CHECK(requestedPosition == expectedGlobal);
    dialog.close();
}

TEST_CASE("day agenda creates inclusive quarter-hour ranges by click and drag",
          "[gui][calendar][agenda]")
{
    javelin::gui::calendar::DayAgendaDialog dialog;
    dialog.setDay(QDate{2026, 8, 10}, {});
    dialog.show();
    settleGui();

    auto* timeline = dialog.findChild<QWidget*>(QStringLiteral("dayAgendaTimeline"));
    REQUIRE(timeline != nullptr);
    std::vector<std::pair<QDateTime, QDateTime>> ranges;
    QObject::connect(&dialog, &javelin::gui::calendar::DayAgendaDialog::newEventRequested, &dialog,
                     [&ranges](const QDateTime& start, const QDateTime& end)
                     { ranges.emplace_back(start, end); });
    const auto positionForBlock = [timeline](const int block)
    { return QPointF{timeline->width() - 10.0, block * 16.0 + 2.0}; };

    sendMouseEvent(timeline, QEvent::MouseButtonPress, positionForBlock(37), Qt::LeftButton,
                   Qt::LeftButton);
    sendMouseEvent(timeline, QEvent::MouseMove, positionForBlock(42), Qt::NoButton, Qt::LeftButton);
    auto* preview = dialog.findChild<javelin::gui::calendar::CalendarEventButton*>(
        QStringLiteral("dayAgendaNewEventButton"));
    REQUIRE(preview != nullptr);
    CHECK(preview->text() == QStringLiteral("New Event"));
    CHECK(preview->property("agendaStartMinute").toInt() == 9 * 60 + 15);
    CHECK(preview->property("agendaEndMinute").toInt() == 10 * 60 + 45);
    sendMouseEvent(timeline, QEvent::MouseButtonRelease, positionForBlock(42), Qt::LeftButton,
                   Qt::NoButton);

    sendMouseEvent(timeline, QEvent::MouseButtonPress, positionForBlock(48), Qt::LeftButton,
                   Qt::LeftButton);
    sendMouseEvent(timeline, QEvent::MouseMove, positionForBlock(39), Qt::NoButton, Qt::LeftButton);
    sendMouseEvent(timeline, QEvent::MouseButtonRelease, positionForBlock(39), Qt::LeftButton,
                   Qt::NoButton);

    sendMouseEvent(timeline, QEvent::MouseButtonPress, positionForBlock(58), Qt::LeftButton,
                   Qt::LeftButton);
    sendMouseEvent(timeline, QEvent::MouseButtonRelease, positionForBlock(58), Qt::LeftButton,
                   Qt::NoButton);

    REQUIRE(ranges.size() == 3);
    CHECK(ranges[0] == std::pair{QDateTime{QDate{2026, 8, 10}, QTime{9, 15}},
                                 QDateTime{QDate{2026, 8, 10}, QTime{10, 45}}});
    CHECK(ranges[1] == std::pair{QDateTime{QDate{2026, 8, 10}, QTime{9, 45}},
                                 QDateTime{QDate{2026, 8, 10}, QTime{12, 15}}});
    CHECK(ranges[2] == std::pair{QDateTime{QDate{2026, 8, 10}, QTime{14, 30}},
                                 QDateTime{QDate{2026, 8, 10}, QTime{15, 30}}});
    CHECK(dialog.findChild<QWidget*>(QStringLiteral("dayAgendaNewEventButton")) == nullptr);
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

TEST_CASE("day agenda keeps the committed day visible until navigation data arrives",
          "[gui][calendar][agenda][navigation]")
{
    javelin::gui::calendar::DayAgendaDialog dialog;
    const auto retained = event(QStringLiteral("retained"), QTime{11, 0}, QTime{12, 0},
                                QStringLiteral("Retained event"));
    dialog.setDay(QDate{2026, 8, 10}, {retained});
    dialog.show();
    settleGui();

    QDate requestedDay;
    QObject::connect(&dialog, &javelin::gui::calendar::DayAgendaDialog::dayChanged, &dialog,
                     [&requestedDay](const QDate& date) { requestedDay = date; });
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
    settleGui();

    CHECK(requestedDay == QDate{2026, 8, 11});
    CHECK(dialog.date() == QDate{2026, 8, 10});
    const auto buttons = dialog.findChildren<QToolButton*>(QStringLiteral("dayAgendaEventButton"));
    REQUIRE(buttons.size() == 1);
    CHECK(buttons.front()->property("agendaEventId").toString() == QStringLiteral("retained"));
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
    QDateTime createStart;
    QDateTime createEnd;
    QObject::connect(&dialog, &javelin::gui::calendar::DayAgendaDialog::dayChanged, &dialog,
                     [&dialog, &requestedDay](const QDate& date)
                     {
                         requestedDay = date;
                         dialog.setDay(date, {});
                     });
    QObject::connect(&dialog, &javelin::gui::calendar::DayAgendaDialog::newEventRequested, &dialog,
                     [&createStart, &createEnd](const QDateTime& start, const QDateTime& end)
                     {
                         createStart = start;
                         createEnd = end;
                     });

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
    CHECK(createStart == QDateTime{QDate{2026, 8, 11}, QTime{9, 0}});
    CHECK(createEnd == QDateTime{QDate{2026, 8, 11}, QTime{10, 0}});
    dialog.close();
}
