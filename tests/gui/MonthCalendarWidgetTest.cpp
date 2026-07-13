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
