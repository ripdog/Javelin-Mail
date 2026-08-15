#include "gui/calendar/CalendarPresentation.h"
#include "gui/calendar/MonthCalendarLayout.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("month calendar layout always presents a locale-aligned 42-day interval",
          "[gui][calendar]")
{
    const QLocale locale{QLocale::English, QLocale::UnitedKingdom};
    const QDate month{2026, 3, 1};
    CHECK(javelin::gui::calendar::monthGridStart(month, locale) == QDate{2026, 2, 23});
    CHECK(javelin::gui::calendar::monthGridCellDate(month, locale, 6) == QDate{2026, 3, 1});
    CHECK(javelin::gui::calendar::monthGridCellDate(month, locale, 41).addDays(1) ==
          QDate{2026, 4, 6});
    CHECK_FALSE(javelin::gui::calendar::monthGridCellDate(month, locale, 42).isValid());
}

TEST_CASE("month calendar layout honors Sunday locale week starts", "[gui][calendar]")
{
    const QLocale locale{QLocale::English, QLocale::UnitedStates};
    CHECK(javelin::gui::calendar::monthGridStart(QDate{2026, 3, 1}, locale) == QDate{2026, 3, 1});
}

TEST_CASE("month calendar event capacity follows cell and font geometry", "[gui][calendar]")
{
    using javelin::gui::calendar::monthCellVisibleEventCount;
    CHECK(monthCellVisibleEventCount(120, 18, 20, 8, 2, 3) == 3);
    CHECK(monthCellVisibleEventCount(120, 18, 20, 8, 2, 6) == 3);
    CHECK(monthCellVisibleEventCount(76, 18, 20, 8, 2, 5) == 1);
    CHECK(monthCellVisibleEventCount(40, 18, 20, 8, 2, 5) == 1);
    CHECK(monthCellVisibleEventCount(120, 18, 28, 8, 2, 6) == 2);
    CHECK(monthCellVisibleEventCount(120, 18, 20, 8, 2, 0) == 0);
}

TEST_CASE("calendar confirmation details identify the event and its date and time",
          "[gui][calendar][confirmation]")
{
    javelin::jmap::calendar::CalendarEvent event;
    event.title = "Planning review";
    event.start = {.value = "2026-08-15T09:30:00"};
    event.duration = {.value = "PT1H"};

    const auto details = javelin::gui::calendar::eventConfirmationDetails(event);
    const auto start = QDateTime::fromString(QStringLiteral("2026-08-15T09:30:00"), Qt::ISODate);
    CHECK(details.contains(QStringLiteral("Planning review")));
    CHECK(details.contains(QLocale{}.toString(start.date(), QLocale::LongFormat)));
    CHECK(details.contains(QLocale{}.toString(start.time(), QLocale::ShortFormat)));
}

TEST_CASE("calendar confirmation details keep all-day events date-only",
          "[gui][calendar][confirmation]")
{
    javelin::jmap::calendar::CalendarEvent event;
    event.title = "Public holiday";
    event.start = {.value = "2026-08-15T00:00:00"};
    event.duration = {.value = "P1D"};
    event.showWithoutTime = true;

    const auto details = javelin::gui::calendar::eventConfirmationDetails(event);
    const auto start = QDateTime::fromString(QStringLiteral("2026-08-15T00:00:00"), Qt::ISODate);
    CHECK(details.contains(QStringLiteral("Public holiday")));
    CHECK(details.contains(QLocale{}.toString(start.date(), QLocale::LongFormat)));
    CHECK_FALSE(details.contains(QLocale{}.toString(start.time(), QLocale::ShortFormat)));
}

TEST_CASE("calendar presentation includes events from subscribed calendars only",
          "[gui][calendar][subscriptions]")
{
    const javelin::jmap::cache::CalendarAccount account{
        .ownerAccountId = "server-1",
        .accountId = "a1",
        .name = "Personal",
    };
    javelin::jmap::calendar::Calendar subscribed;
    subscribed.accountId = "a1";
    subscribed.id = "subscribed";
    subscribed.name = "Subscribed";
    subscribed.color = "#336699";
    subscribed.isSubscribed = true;
    subscribed.isVisible = false;
    javelin::jmap::calendar::Calendar unsubscribed;
    unsubscribed.accountId = "a1";
    unsubscribed.id = "unsubscribed";
    unsubscribed.name = "Unsubscribed";
    unsubscribed.color = "#993366";
    unsubscribed.sortOrder = 1;
    unsubscribed.isSubscribed = false;
    unsubscribed.isVisible = true;
    const std::vector calendars{subscribed, unsubscribed};

    javelin::jmap::calendar::CalendarEvent visibleEvent;
    visibleEvent.accountId = "a1";
    visibleEvent.id = "visible-event";
    visibleEvent.uid = "uid-visible";
    visibleEvent.calendarIds = {{"subscribed", true}};
    visibleEvent.title = "Visible";
    visibleEvent.start = {.value = "2026-08-05T09:00:00"};
    visibleEvent.duration = {.value = "PT1H"};
    javelin::jmap::calendar::CalendarEvent hiddenEvent;
    hiddenEvent.accountId = "a1";
    hiddenEvent.id = "hidden-event";
    hiddenEvent.uid = "uid-hidden";
    hiddenEvent.calendarIds = {{"unsubscribed", true}};
    hiddenEvent.title = "Hidden";
    hiddenEvent.start = {.value = "2026-08-06T09:00:00"};
    hiddenEvent.duration = {.value = "PT1H"};

    javelin::jmap::calendar::Occurrence visibleOccurrence;
    visibleOccurrence.accountId = "a1";
    visibleOccurrence.id = "visible-event";
    visibleOccurrence.eventId = "visible-event";
    visibleOccurrence.localStart = {.value = "2026-08-05T09:00:00"};
    visibleOccurrence.localEnd = {.value = "2026-08-05T10:00:00"};
    javelin::jmap::calendar::Occurrence hiddenOccurrence;
    hiddenOccurrence.accountId = "a1";
    hiddenOccurrence.id = "hidden-event";
    hiddenOccurrence.eventId = "hidden-event";
    hiddenOccurrence.localStart = {.value = "2026-08-06T09:00:00"};
    hiddenOccurrence.localEnd = {.value = "2026-08-06T10:00:00"};

    javelin::jmap::cache::CalendarWindow window{
        .accountId = "a1",
        .start = {.value = "2026-08-01T00:00:00"},
        .end = {.value = "2026-09-01T00:00:00"},
        .displayTimeZone = {.value = "Pacific/Auckland"},
        .queryState = "q1",
        .eventState = "e1",
        .events = {visibleEvent, hiddenEvent},
        .occurrences = {visibleOccurrence, hiddenOccurrence},
    };

    const auto presentation = javelin::gui::calendar::buildCalendarAccountPresentation(
        account, calendars, window, QColor{Qt::blue});

    REQUIRE(presentation.calendars.size() == 2);
    CHECK(presentation.calendars[0].name == QStringLiteral("Subscribed"));
    CHECK(presentation.calendars[0].subscribed);
    CHECK_FALSE(presentation.calendars[1].subscribed);
    REQUIRE(presentation.events.size() == 1);
    CHECK(presentation.events.front().eventId == "visible-event");
}

TEST_CASE("new event destination prefers the configured global calendar across accounts",
          "[gui][calendar][default-destination]")
{
    const std::vector<javelin::gui::calendar::NewEventCalendarCandidate> candidates{
        {.ownerAccountId = "server-1",
         .accountId = "a1",
         .calendarId = "personal",
         .writable = true,
         .serverDefault = true},
        {.ownerAccountId = "server-2",
         .accountId = "a1",
         .calendarId = "work",
         .writable = true,
         .serverDefault = true},
    };

    const javelin::protocol::CalendarDefaultDestination configured{
        .ownerAccountId = QStringLiteral("server-2"),
        .accountId = QStringLiteral("a1"),
        .calendarId = QStringLiteral("work"),
    };
    CHECK(javelin::gui::calendar::preferredNewEventCalendarIndex(candidates, configured) ==
          std::optional<std::size_t>{1});

    CHECK(javelin::gui::calendar::preferredNewEventCalendarIndex(candidates, {}) ==
          std::optional<std::size_t>{0});
}

TEST_CASE("new event destination falls back when the configured calendar is not writable",
          "[gui][calendar][default-destination]")
{
    const std::vector<javelin::gui::calendar::NewEventCalendarCandidate> candidates{
        {.ownerAccountId = "server-1",
         .accountId = "a1",
         .calendarId = "personal",
         .writable = true,
         .serverDefault = true},
        {.ownerAccountId = "server-2",
         .accountId = "a2",
         .calendarId = "locked",
         .writable = false,
         .serverDefault = false},
    };
    const javelin::protocol::CalendarDefaultDestination configured{
        .ownerAccountId = QStringLiteral("server-2"),
        .accountId = QStringLiteral("a2"),
        .calendarId = QStringLiteral("locked"),
    };

    CHECK(javelin::gui::calendar::preferredNewEventCalendarIndex(candidates, configured) ==
          std::optional<std::size_t>{0});
}

TEST_CASE("month calendar labels multi-day event segments coherently", "[gui][calendar]")
{
    using javelin::gui::calendar::monthEventSegment;
    const QDateTime start{QDate{2026, 7, 13}, QTime{9, 0}};
    const QDateTime end{QDate{2026, 7, 15}, QTime{0, 0}};
    CHECK(monthEventSegment(QStringLiteral("Trip"), start, end, true, QDate{2026, 7, 13}).label ==
          QStringLiteral("Trip →"));
    CHECK(monthEventSegment(QStringLiteral("Trip"), start, end, true, QDate{2026, 7, 14}).label ==
          QStringLiteral("← Trip"));
    CHECK(
        monthEventSegment(QStringLiteral("Deploy"), start, end, false, QDate{2026, 7, 13}).label ==
        QStringLiteral("09:00 Deploy →"));
    CHECK(
        monthEventSegment(QStringLiteral("Deploy"), start, end, false, QDate{2026, 7, 14}).label ==
        QStringLiteral("← Deploy"));
}
