#pragma once

#include <QDate>
#include <QDateTime>
#include <QLocale>
#include <QString>

#include <cstddef>

namespace javelin::gui::calendar
{
    struct MonthEventSegment
    {
        QString label;
        bool begins = false;
        bool ends = false;
    };

    [[nodiscard]] QDate monthGridStart(const QDate& month, const QLocale& locale);
    [[nodiscard]] QDate monthGridCellDate(const QDate& month, const QLocale& locale, int index);
    [[nodiscard]] std::size_t monthCellVisibleEventCount(int cellHeight, int headerHeight,
                                                         int eventRowHeight, int verticalMargins,
                                                         int spacing, std::size_t eventCount);
    [[nodiscard]] QDate monthEventLastDate(const QDateTime& start, const QDateTime& end);
    [[nodiscard]] MonthEventSegment monthEventSegment(const QString& title, const QDateTime& start,
                                                      const QDateTime& end, bool allDay,
                                                      const QDate& cellDate);
} // namespace javelin::gui::calendar
