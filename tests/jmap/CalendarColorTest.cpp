#include "jmap/calendar/CalendarColor.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("calendar colors accept CSS Color 3 names case-insensitively", "[jmap][calendar][color]")
{
    CHECK(javelin::jmap::calendar::isValidCalendarColor("red"));
    CHECK(javelin::jmap::calendar::isValidCalendarColor("DarkSlateGrey"));
    CHECK(javelin::jmap::calendar::isValidCalendarColor("LIGHTGOLDENRODYELLOW"));
}

TEST_CASE("calendar colors accept CSS Color 3 hexadecimal notation", "[jmap][calendar][color]")
{
    CHECK(javelin::jmap::calendar::isValidCalendarColor("#abc"));
    CHECK(javelin::jmap::calendar::isValidCalendarColor("#A1b2C3"));
}

TEST_CASE("calendar colors reject values outside the JMAP Calendars color grammar",
          "[jmap][calendar][color]")
{
    CHECK_FALSE(javelin::jmap::calendar::isValidCalendarColor(""));
    CHECK_FALSE(javelin::jmap::calendar::isValidCalendarColor("#abcd"));
    CHECK_FALSE(javelin::jmap::calendar::isValidCalendarColor("#12345678"));
    CHECK_FALSE(javelin::jmap::calendar::isValidCalendarColor("rgb(1, 2, 3)"));
    CHECK_FALSE(javelin::jmap::calendar::isValidCalendarColor("rebeccapurple"));
    CHECK_FALSE(javelin::jmap::calendar::isValidCalendarColor("red "));
}
