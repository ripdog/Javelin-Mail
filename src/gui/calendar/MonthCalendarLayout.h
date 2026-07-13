#pragma once

#include <QDate>
#include <QLocale>

namespace javelin::gui::calendar
{
    [[nodiscard]] QDate monthGridStart(const QDate& month, const QLocale& locale);
    [[nodiscard]] QDate monthGridCellDate(const QDate& month, const QLocale& locale, int index);
} // namespace javelin::gui::calendar
