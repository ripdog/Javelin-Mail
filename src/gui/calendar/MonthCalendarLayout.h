#pragma once

#include <QDate>
#include <QLocale>

#include <cstddef>

namespace javelin::gui::calendar
{
    [[nodiscard]] QDate monthGridStart(const QDate& month, const QLocale& locale);
    [[nodiscard]] QDate monthGridCellDate(const QDate& month, const QLocale& locale, int index);
    [[nodiscard]] std::size_t monthCellVisibleEventCount(int cellHeight, int headerHeight,
                                                         int eventRowHeight, int verticalMargins,
                                                         int spacing, std::size_t eventCount);
} // namespace javelin::gui::calendar
