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
