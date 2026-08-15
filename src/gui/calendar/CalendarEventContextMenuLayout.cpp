#include "gui/calendar/CalendarEventContextMenuLayout.h"

#include <QSet>

#include <algorithm>

namespace javelin::gui::calendar
{
    const QString& calendarEventContextMenuSeparatorId()
    {
        static const QString id = QStringLiteral("separator");
        return id;
    }

    const std::vector<QString>& supportedCalendarEventContextMenuActionIds()
    {
        static const std::vector<QString> ids = {
            QStringLiteral("calendar_event_edit"),
            QStringLiteral("calendar_event_duplicate"),
            QStringLiteral("calendar_event_move"),
            QStringLiteral("calendar_event_accept"),
            QStringLiteral("calendar_event_tentative"),
            QStringLiteral("calendar_event_decline"),
            QStringLiteral("calendar_event_copy_details"),
            QStringLiteral("calendar_event_delete"),
        };
        return ids;
    }

    const std::vector<QString>& defaultCalendarEventContextMenuLayout()
    {
        static const std::vector<QString> layout = {
            QStringLiteral("calendar_event_edit"),
            QStringLiteral("calendar_event_duplicate"),
            calendarEventContextMenuSeparatorId(),
            QStringLiteral("calendar_event_move"),
            calendarEventContextMenuSeparatorId(),
            QStringLiteral("calendar_event_accept"),
            QStringLiteral("calendar_event_tentative"),
            QStringLiteral("calendar_event_decline"),
            calendarEventContextMenuSeparatorId(),
            QStringLiteral("calendar_event_copy_details"),
            calendarEventContextMenuSeparatorId(),
            QStringLiteral("calendar_event_delete"),
        };
        return layout;
    }

    std::vector<QString> normalizeCalendarEventContextMenuLayout(const std::vector<QString>& layout)
    {
        const auto& supported = supportedCalendarEventContextMenuActionIds();
        QSet<QString> seenActions;
        std::vector<QString> normalized;
        normalized.reserve(layout.size());
        for (const auto& id : layout)
        {
            if (id == calendarEventContextMenuSeparatorId())
            {
                if (!normalized.empty() &&
                    normalized.back() != calendarEventContextMenuSeparatorId())
                    normalized.push_back(id);
                continue;
            }
            if (!std::ranges::contains(supported, id) || seenActions.contains(id))
                continue;
            seenActions.insert(id);
            normalized.push_back(id);
        }
        if (!normalized.empty() && normalized.back() == calendarEventContextMenuSeparatorId())
            normalized.pop_back();
        return normalized;
    }

    std::vector<QString>
    effectiveCalendarEventContextMenuLayout(const std::vector<QString>& configuredLayout)
    {
        return normalizeCalendarEventContextMenuLayout(
            configuredLayout.empty() ? defaultCalendarEventContextMenuLayout() : configuredLayout);
    }

    std::vector<QString>
    calendarEventContextMenuOverrideForLayout(const std::vector<QString>& layout)
    {
        auto normalized = normalizeCalendarEventContextMenuLayout(layout);
        if (normalized == defaultCalendarEventContextMenuLayout())
            normalized.clear();
        return normalized;
    }

    std::vector<QString>
    visibleCalendarEventContextMenuLayout(const std::vector<QString>& configuredLayout,
                                          const CalendarEventContextMenuAvailability& availability)
    {
        auto layout = effectiveCalendarEventContextMenuLayout(configuredLayout);
        std::erase_if(layout,
                      [&availability](const QString& id)
                      {
                          if (id == QStringLiteral("calendar_event_edit") ||
                              id == QStringLiteral("calendar_event_delete"))
                              return !availability.editable;
                          if (id == QStringLiteral("calendar_event_duplicate"))
                              return !availability.duplicable;
                          if (id == QStringLiteral("calendar_event_move"))
                              return !availability.movable;
                          if (id == QStringLiteral("calendar_event_accept") ||
                              id == QStringLiteral("calendar_event_tentative") ||
                              id == QStringLiteral("calendar_event_decline"))
                              return !availability.rsvp || !availability.responsePending;
                          return false;
                      });
        return normalizeCalendarEventContextMenuLayout(layout);
    }
} // namespace javelin::gui::calendar
