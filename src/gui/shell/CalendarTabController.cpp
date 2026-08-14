#include "gui/shell/CalendarTabController.h"

#include "gui/calendar/CalendarEventContextMenuLayout.h"
#include "gui/calendar/CalendarPresentation.h"
#include "gui/calendar/DayAgendaDialog.h"
#include "gui/calendar/EventDialog.h"
#include "gui/calendar/MonthCalendarWidget.h"
#include "gui/settings/GuiSettings.h"
#include "jmap/calendar/CalendarEventEditing.h"
#include "jmap/calendar/CalendarReader.h"

#include <QCoroTask>

#include <KLocalizedString>

#include <QAbstractButton>
#include <QAction>
#include <QClipboard>
#include <QDateTime>
#include <QDebug>
#include <QGuiApplication>
#include <QLocale>
#include <QLoggingCategory>
#include <QMenu>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QStackedWidget>
#include <QTime>
#include <QTimeZone>
#include <QTimer>

#include <iterator>
#include <ranges>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace javelin::gui::shell
{
    Q_LOGGING_CATEGORY(logCalendarOperations, "user.operations")

    namespace
    {
        [[nodiscard]] QString
        calendarAccountLabel(const javelin::gui::settings::GuiSettings& settings,
                             const javelin::jmap::cache::CalendarAccount& account)
        {
            const auto configured =
                settings.accountForCachedId(QString::fromStdString(account.accountId));
            if (!configured.displayName.isEmpty())
                return configured.displayName;
            return account.name.empty() ? i18n("Unnamed account")
                                        : QString::fromStdString(account.name);
        }

        [[nodiscard]] QString displayCalendarAddress(const std::string_view address)
        {
            auto value = QString::fromUtf8(address.data(), static_cast<qsizetype>(address.size()));
            if (value.startsWith(QStringLiteral("mailto:"), Qt::CaseInsensitive))
                value.remove(0, 7);
            return value;
        }

        [[nodiscard]] QString participantLabel(const javelin::jmap::calendar::Attendee& participant)
        {
            const auto address = displayCalendarAddress(participant.calendarAddress);
            const auto name = QString::fromStdString(participant.name);
            if (name.isEmpty())
                return address;
            if (address.isEmpty() || name.compare(address, Qt::CaseInsensitive) == 0)
                return name;
            return i18nc("calendar participant display", "%1 <%2>", name, address);
        }

        [[nodiscard]] QString
        invitationOrganizer(const javelin::jmap::calendar::CalendarEvent& event)
        {
            const auto owner = std::ranges::find_if(event.attendees, [](const auto& participant)
                                                    { return participant.isOwner; });
            if (owner != event.attendees.end())
                return participantLabel(*owner);
            if (event.organizerCalendarAddress)
                return displayCalendarAddress(*event.organizerCalendarAddress);
            return i18n("Unknown organizer");
        }

        [[nodiscard]] QString invitationWhen(const javelin::jmap::calendar::CalendarEvent& event)
        {
            const auto start =
                QDateTime::fromString(QString::fromStdString(event.start.value), Qt::ISODate);
            if (!start.isValid())
                return QString::fromStdString(event.start.value);
            if (event.showWithoutTime)
                return QLocale().toString(start.date(), QLocale::ShortFormat);
            auto value = i18nc("calendar event date and time", "%1 at %2",
                               QLocale().toString(start.date(), QLocale::ShortFormat),
                               QLocale().toString(start.time(), QLocale::ShortFormat));
            if (event.timeZone)
                value += i18nc("calendar event time zone", " (%1)",
                               QString::fromStdString(event.timeZone->value));
            return value;
        }

        [[nodiscard]] bool canEditEvent(const javelin::jmap::calendar::CalendarEvent& event,
                                        const std::string_view configuredAddress)
        {
            return event.isOrigin ||
                   javelin::jmap::calendar::eventOwnedByAddress(event, configuredAddress);
        }

        [[nodiscard]] bool canRsvp(const javelin::jmap::calendar::CalendarEvent& event,
                                   const std::vector<javelin::jmap::calendar::Calendar>& calendars,
                                   const std::string_view configuredAddress)
        {
            const auto participant =
                javelin::jmap::calendar::participantIndexForAddress(event, configuredAddress);
            if (!participant || event.attendees[*participant].isOwner)
                return false;
            std::unordered_set<std::string> required;
            for (const auto& [calendarId, present] : event.calendarIds)
                if (present)
                    required.emplace(calendarId);
            for (const auto& calendar : calendars)
            {
                if (!required.erase(calendar.id))
                    continue;
                if (!calendar.myRights.mayRSVP)
                    return false;
            }
            return required.empty();
        }

        [[nodiscard]] std::optional<std::string>
        promptInvitationResponse(QWidget* parent,
                                 const javelin::jmap::calendar::CalendarEvent& event,
                                 const bool responseAllowed)
        {
            QStringList details{
                i18n("Organizer: %1", invitationOrganizer(event)),
                i18n("When: %1", invitationWhen(event)),
            };
            if (event.location && !event.location->empty())
                details.push_back(i18n("Where: %1", QString::fromStdString(*event.location)));
            QStringList attendees;
            for (const auto& participant : event.attendees)
                if (participant.isAttendee && !participant.isOwner)
                    attendees.push_back(participantLabel(participant));
            if (!attendees.isEmpty())
                details.push_back(i18n("Attendees: %1", attendees.join(QStringLiteral(", "))));
            if (event.description && !event.description->empty())
                details.push_back(QStringLiteral("\n") +
                                  QString::fromStdString(*event.description));
            if (!responseAllowed)
                details.push_back(
                    QStringLiteral("\n") +
                    i18n("This calendar does not allow you to respond to the invitation."));

            QMessageBox prompt{QMessageBox::Information, i18n("Calendar invitation"),
                               event.title.empty() ? i18n("Untitled event")
                                                   : QString::fromStdString(event.title),
                               QMessageBox::NoButton, parent};
            prompt.setInformativeText(details.join(QLatin1Char('\n')));
            QPushButton* accept = nullptr;
            QPushButton* tentative = nullptr;
            QPushButton* decline = nullptr;
            if (responseAllowed)
            {
                accept = prompt.addButton(i18nc("@action:button calendar RSVP", "Accept"),
                                          QMessageBox::AcceptRole);
                tentative = prompt.addButton(i18nc("@action:button calendar RSVP", "Tentative"),
                                             QMessageBox::ActionRole);
                decline = prompt.addButton(i18nc("@action:button calendar RSVP", "Decline"),
                                           QMessageBox::DestructiveRole);
            }
            auto* close = prompt.addButton(QMessageBox::Close);
            prompt.setDefaultButton(close);
            prompt.setEscapeButton(close);
            prompt.exec();
            if (prompt.clickedButton() == accept)
                return std::string{"accepted"};
            if (prompt.clickedButton() == tentative)
                return std::string{"tentative"};
            if (prompt.clickedButton() == decline)
                return std::string{"declined"};
            return std::nullopt;
        }

        [[nodiscard]] std::vector<javelin::gui::calendar::DayAgendaEvent>
        buildDayAgendaEvents(javelin::gui::settings::GuiSettings& settings,
                             javelin::jmap::calendar::CalendarReader& calendarReader,
                             javelin::gui::calendar::MonthCalendarWidget& widget,
                             const std::vector<javelin::jmap::cache::CalendarAccount>& accounts,
                             const QDate& date)
        {
            std::vector<javelin::gui::calendar::DayAgendaEvent> result;
            const auto displayEvents = widget.eventsForDate(date);
            result.reserve(displayEvents.size());
            if (displayEvents.empty())
                return result;

            const javelin::jmap::calendar::VisibleInterval interval{
                .start = {.value = widget.visibleStart().toString(Qt::ISODate).toStdString() +
                                   "T00:00:00"},
                .end = {.value =
                            widget.visibleEnd().toString(Qt::ISODate).toStdString() + "T00:00:00"}};
            const javelin::jmap::calendar::TimeZoneId timeZone{
                .value = QTimeZone::systemTimeZoneId().toStdString()};

            for (const auto& account : accounts)
            {
                if (std::ranges::none_of(displayEvents, [&account](const auto& event)
                                         { return event.accountId == account.accountId; }))
                    continue;

                const auto listed = calendarReader.calendars(account.accountId);
                const auto* calendars =
                    std::get_if<std::vector<javelin::jmap::calendar::Calendar>>(&listed);
                const auto loaded =
                    calendarReader.loadCached(account.accountId, interval, timeZone);
                const auto* window =
                    std::get_if<std::optional<javelin::jmap::cache::CalendarWindow>>(&loaded);
                const auto configuredAddress =
                    settings.accountForCachedId(QString::fromStdString(account.accountId))
                        .loginEmail.toStdString();

                for (const auto& displayEvent : displayEvents)
                {
                    if (displayEvent.accountId != account.accountId)
                        continue;

                    const javelin::jmap::calendar::CalendarEvent* event = nullptr;
                    if (window != nullptr && window->has_value())
                    {
                        const auto found =
                            std::ranges::find(window->value().events, displayEvent.eventId,
                                              &javelin::jmap::calendar::CalendarEvent::id);
                        if (found != window->value().events.end())
                            event = &*found;
                    }

                    const javelin::jmap::calendar::Calendar* calendar = nullptr;
                    if (calendars != nullptr)
                    {
                        const auto separator = displayEvent.calendarId.find('\n');
                        if (separator != std::string::npos)
                        {
                            const auto calendarId = displayEvent.calendarId.substr(separator + 1);
                            const auto found = std::ranges::find(
                                *calendars, calendarId, &javelin::jmap::calendar::Calendar::id);
                            if (found != calendars->end())
                                calendar = &*found;
                        }
                    }

                    QString calendarName =
                        calendar != nullptr ? QString::fromStdString(calendar->name) : QString{};
                    if (accounts.size() > 1 && !calendarName.isEmpty())
                    {
                        calendarName = i18nc("calendar and account", "%1 — %2", calendarName,
                                             calendarAccountLabel(settings, account));
                    }

                    QString organizer;
                    QStringList attendees;
                    if (event != nullptr)
                    {
                        if (event->organizerCalendarAddress ||
                            std::ranges::any_of(event->attendees, [](const auto& attendee)
                                                { return attendee.isOwner; }))
                            organizer = invitationOrganizer(*event);
                        for (const auto& attendee : event->attendees)
                        {
                            if (attendee.isAttendee && !attendee.isOwner)
                                attendees.push_back(participantLabel(attendee));
                        }
                    }

                    result.push_back(javelin::gui::calendar::DayAgendaEvent{
                        .key =
                            {
                                .accountId = QString::fromStdString(displayEvent.accountId),
                                .eventId = QString::fromStdString(displayEvent.eventId),
                                .recurrenceId = QString::fromStdString(
                                    displayEvent.recurrenceId.value_or(std::string{})),
                            },
                        .title = displayEvent.title.isEmpty() ? i18n("Untitled event")
                                                              : displayEvent.title,
                        .calendarName = std::move(calendarName),
                        .color = displayEvent.color,
                        .start = displayEvent.start,
                        .end = displayEvent.end,
                        .allDay = displayEvent.allDay,
                        .recurring = displayEvent.recurring,
                        .editable =
                            event != nullptr && calendar != nullptr &&
                            canEditEvent(*event, configuredAddress) &&
                            (calendar->myRights.mayWriteAll || calendar->myRights.mayWriteOwn),
                        .invitation = event != nullptr && calendars != nullptr &&
                                      !canEditEvent(*event, configuredAddress) &&
                                      canRsvp(*event, *calendars, configuredAddress),
                        .organizer = std::move(organizer),
                        .location = event != nullptr && event->location
                                        ? QString::fromStdString(*event->location)
                                        : QString{},
                        .description = event != nullptr && event->description
                                           ? QString::fromStdString(*event->description)
                                           : QString{},
                        .attendees = std::move(attendees),
                    });
                }
            }

            std::ranges::sort(result,
                              [](const auto& left, const auto& right)
                              {
                                  if (left.allDay != right.allDay)
                                      return left.allDay;
                                  if (left.start != right.start)
                                      return left.start < right.start;
                                  return left.title.localeAwareCompare(right.title) < 0;
                              });
            return result;
        }
    } // namespace

    CalendarTabController::CalendarTabController(
        javelin::gui::settings::GuiSettings& settings,
        javelin::jmap::calendar::CalendarReader& calendarReader,
        javelin::app::CalendarCommandPort& calendarCommandPort, QStackedWidget& contentStack,
        std::vector<TabState>& tabs, QObject* parent)
        : QObject(parent), m_settings(settings), m_calendarReader(calendarReader),
          m_calendarCommandPort(calendarCommandPort), m_contentStack(contentStack), m_tabs(tabs)
    {
    }

    void CalendarTabController::configureEventContextMenu(
        QMenu& menu, std::function<std::vector<QString>()> configuredLayout,
        std::function<void(const QList<QAction*>&)> replaceActionList,
        CalendarEventContextActions actions)
    {
        m_eventContextMenu = &menu;
        m_configuredEventContextMenuLayout = std::move(configuredLayout);
        m_replaceEventContextMenuActionList = std::move(replaceActionList);
        m_eventContextActions.emplace(actions);

        const auto request = [this](const QString& actionId)
        {
            if (m_eventContextWidget == nullptr)
                return;
            Q_EMIT m_eventContextWidget->eventContextActionRequested(
                actionId, m_eventContextAccountId, m_eventContextEventId,
                m_eventContextRecurrenceId, {});
        };
        connect(&actions.edit, &QAction::triggered, this,
                [request] { request(QStringLiteral("calendar_event_edit")); });
        connect(&actions.duplicate, &QAction::triggered, this,
                [request] { request(QStringLiteral("calendar_event_duplicate")); });
        connect(&actions.accept, &QAction::triggered, this,
                [request] { request(QStringLiteral("calendar_event_accept")); });
        connect(&actions.tentative, &QAction::triggered, this,
                [request] { request(QStringLiteral("calendar_event_tentative")); });
        connect(&actions.decline, &QAction::triggered, this,
                [request] { request(QStringLiteral("calendar_event_decline")); });
        connect(&actions.copyDetails, &QAction::triggered, this,
                [request] { request(QStringLiteral("calendar_event_copy_details")); });
        connect(&actions.deleteEvent, &QAction::triggered, this,
                [request] { request(QStringLiteral("calendar_event_delete")); });
    }

    void CalendarTabController::showEventContextMenu(
        javelin::gui::calendar::MonthCalendarWidget& widget, const QPoint& globalPosition,
        const QString& accountId, const QString& eventId, const QString& recurrenceId)
    {
        if (m_eventContextMenu == nullptr || !m_configuredEventContextMenuLayout ||
            !m_replaceEventContextMenuActionList || !m_eventContextActions)
            return;

        const auto accountsResult = m_calendarReader.accounts();
        const auto* accounts =
            std::get_if<std::vector<javelin::jmap::cache::CalendarAccount>>(&accountsResult);
        if (accounts == nullptr)
            return;
        const auto account = std::ranges::find(*accounts, accountId.toStdString(),
                                               &javelin::jmap::cache::CalendarAccount::accountId);
        if (account == accounts->end())
            return;

        const javelin::jmap::calendar::VisibleInterval interval{
            .start = {.value =
                          widget.visibleStart().toString(Qt::ISODate).toStdString() + "T00:00:00"},
            .end = {.value =
                        widget.visibleEnd().toString(Qt::ISODate).toStdString() + "T00:00:00"}};
        const javelin::jmap::calendar::TimeZoneId timeZone{
            .value = QTimeZone::systemTimeZoneId().toStdString()};
        const auto loaded = m_calendarReader.loadCached(account->accountId, interval, timeZone);
        const auto* window =
            std::get_if<std::optional<javelin::jmap::cache::CalendarWindow>>(&loaded);
        if (window == nullptr || !window->has_value())
            return;
        const auto event = std::ranges::find(window->value().events, eventId.toStdString(),
                                             &javelin::jmap::calendar::CalendarEvent::id);
        if (event == window->value().events.end())
            return;
        const auto calendarsResult = m_calendarReader.calendars(account->accountId);
        const auto* calendars =
            std::get_if<std::vector<javelin::jmap::calendar::Calendar>>(&calendarsResult);
        if (calendars == nullptr)
            return;

        const auto writable = [](const javelin::jmap::calendar::Calendar& calendar)
        { return calendar.myRights.mayWriteAll || calendar.myRights.mayWriteOwn; };
        const auto configuredAddress =
            m_settings.accountForCachedId(QString::fromStdString(account->accountId))
                .loginEmail.toStdString();
        const bool editable =
            canEditEvent(*event, configuredAddress) &&
            std::ranges::any_of(*calendars,
                                [&event, &writable](const auto& calendar)
                                {
                                    const auto membership = event->calendarIds.find(calendar.id);
                                    return membership != event->calendarIds.end() &&
                                           membership->second && writable(calendar);
                                });
        const bool rsvp = !canEditEvent(*event, configuredAddress) &&
                          canRsvp(*event, *calendars, configuredAddress);
        const bool canDuplicate =
            std::ranges::any_of(*calendars, [&writable](const auto& calendar)
                                { return calendar.isSubscribed && writable(calendar); });

        m_replaceEventContextMenuActionList({});
        qDeleteAll(m_eventContextMenuObjects);
        m_eventContextMenuObjects.clear();
        m_eventContextWidget = &widget;
        m_eventContextAccountId = accountId;
        m_eventContextEventId = eventId;
        m_eventContextRecurrenceId = recurrenceId;

        QMenu* moveMenu = nullptr;
        const auto actionForId = [&](const QString& id) -> QAction*
        {
            if (id == QStringLiteral("calendar_event_edit"))
                return editable ? &m_eventContextActions->edit : nullptr;
            if (id == QStringLiteral("calendar_event_duplicate"))
                return canDuplicate ? &m_eventContextActions->duplicate : nullptr;
            if (id == QStringLiteral("calendar_event_move"))
            {
                if (!editable)
                    return nullptr;
                moveMenu = new QMenu(i18n("Move to Calendar"), m_eventContextMenu);
                moveMenu->setIcon(QIcon::fromTheme(QStringLiteral("mail-move")));
                m_eventContextMenuObjects.push_back(moveMenu);
                for (const auto& calendar : *calendars)
                {
                    const auto membership = event->calendarIds.find(calendar.id);
                    if (!calendar.isSubscribed || !writable(calendar) ||
                        (membership != event->calendarIds.end() && membership->second))
                        continue;
                    auto* destination = moveMenu->addAction(QString::fromStdString(calendar.name));
                    connect(destination, &QAction::triggered, this,
                            [this, calendarId = QString::fromStdString(calendar.id)]
                            {
                                if (m_eventContextWidget != nullptr)
                                    Q_EMIT m_eventContextWidget->eventContextActionRequested(
                                        QStringLiteral("calendar_event_move"),
                                        m_eventContextAccountId, m_eventContextEventId,
                                        m_eventContextRecurrenceId, calendarId);
                            });
                }
                return moveMenu->actions().empty() ? nullptr : moveMenu->menuAction();
            }
            if (id == QStringLiteral("calendar_event_accept"))
                return rsvp ? &m_eventContextActions->accept : nullptr;
            if (id == QStringLiteral("calendar_event_tentative"))
                return rsvp ? &m_eventContextActions->tentative : nullptr;
            if (id == QStringLiteral("calendar_event_decline"))
                return rsvp ? &m_eventContextActions->decline : nullptr;
            if (id == QStringLiteral("calendar_event_copy_details"))
                return &m_eventContextActions->copyDetails;
            if (id == QStringLiteral("calendar_event_delete"))
                return editable ? &m_eventContextActions->deleteEvent : nullptr;
            return nullptr;
        };

        QList<QAction*> actions;
        bool separatorPending = false;
        for (const auto& id : javelin::gui::calendar::visibleCalendarEventContextMenuLayout(
                 m_configuredEventContextMenuLayout(), {.editable = editable,
                                                        .duplicable = canDuplicate,
                                                        .movable = editable,
                                                        .rsvp = rsvp}))
        {
            if (id == javelin::gui::calendar::calendarEventContextMenuSeparatorId())
            {
                separatorPending = !actions.empty();
                continue;
            }
            auto* action = actionForId(id);
            if (action == nullptr)
                continue;
            if (separatorPending)
            {
                auto* separator = new QAction(m_eventContextMenu);
                separator->setSeparator(true);
                m_eventContextMenuObjects.push_back(separator);
                actions.push_back(separator);
                separatorPending = false;
            }
            actions.push_back(action);
        }
        m_replaceEventContextMenuActionList(actions);
        m_eventContextMenu->exec(globalPosition);
    }

    void CalendarTabController::handleEventContextAction(
        javelin::gui::calendar::MonthCalendarWidget& widget, const QString& actionId,
        const QString& accountId, const QString& eventId, const QString& recurrenceId,
        const QString& targetCalendarId)
    {
        if (actionId == QStringLiteral("calendar_event_edit"))
        {
            Q_EMIT widget.eventActivated(accountId, eventId, recurrenceId);
            return;
        }

        const auto accountsResult = m_calendarReader.accounts();
        const auto* accounts =
            std::get_if<std::vector<javelin::jmap::cache::CalendarAccount>>(&accountsResult);
        if (accounts == nullptr)
            return;
        const auto account = std::ranges::find(*accounts, accountId.toStdString(),
                                               &javelin::jmap::cache::CalendarAccount::accountId);
        if (account == accounts->end())
            return;
        const javelin::jmap::calendar::VisibleInterval interval{
            .start = {.value =
                          widget.visibleStart().toString(Qt::ISODate).toStdString() + "T00:00:00"},
            .end = {.value =
                        widget.visibleEnd().toString(Qt::ISODate).toStdString() + "T00:00:00"}};
        const javelin::jmap::calendar::TimeZoneId timeZone{
            .value = QTimeZone::systemTimeZoneId().toStdString()};
        const auto loaded = m_calendarReader.loadCached(account->accountId, interval, timeZone);
        const auto* window =
            std::get_if<std::optional<javelin::jmap::cache::CalendarWindow>>(&loaded);
        if (window == nullptr || !window->has_value())
            return;
        const auto foundEvent = std::ranges::find(window->value().events, eventId.toStdString(),
                                                  &javelin::jmap::calendar::CalendarEvent::id);
        if (foundEvent == window->value().events.end())
            return;
        const auto calendarsResult = m_calendarReader.calendars(account->accountId);
        const auto* calendars =
            std::get_if<std::vector<javelin::jmap::calendar::Calendar>>(&calendarsResult);
        if (calendars == nullptr)
            return;
        const auto configuredAddress =
            m_settings.accountForCachedId(QString::fromStdString(account->accountId))
                .loginEmail.toStdString();

        auto selectedEvent = *foundEvent;
        if (!recurrenceId.isEmpty())
        {
            const auto occurrence = std::ranges::find_if(
                window->value().occurrences,
                [&eventId, &recurrenceId](const auto& candidate)
                {
                    return candidate.eventId == eventId.toStdString() && candidate.recurrenceId &&
                           candidate.recurrenceId->value == recurrenceId.toStdString();
                });
            if (occurrence == window->value().occurrences.end())
            {
                Q_EMIT statusMessage(
                    i18n("This occurrence is no longer available. Refresh and try again."), 10000);
                return;
            }
            selectedEvent.start = occurrence->localStart;
            const auto start = QDateTime::fromString(
                QString::fromStdString(occurrence->localStart.value), Qt::ISODate);
            const auto end = QDateTime::fromString(
                QString::fromStdString(occurrence->localEnd.value), Qt::ISODate);
            selectedEvent.duration.value =
                (occurrence->allDay ? QStringLiteral("P%1D").arg(start.date().daysTo(end.date()))
                                    : QStringLiteral("PT%1S").arg(start.secsTo(end)))
                    .toStdString();
            if (const auto existingOverride =
                    foundEvent->recurrenceOverrides.find(recurrenceId.toStdString());
                existingOverride != foundEvent->recurrenceOverrides.end() &&
                existingOverride->second.title)
                selectedEvent.title = *existingOverride->second.title;
        }

        const bool requiresMaterialization =
            foundEvent->recurrenceRule.has_value() || !foundEvent->recurrenceOverrides.empty();
        const auto materialization =
            requiresMaterialization
                ? std::optional{javelin::jmap::calendar::CalendarRangeMaterialization{
                      .interval = interval,
                      .displayTimeZone = timeZone,
                  }}
                : std::nullopt;
        const auto reportResult = [this](javelin::jmap::calendar::CalendarMutationResult result)
        {
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                Q_EMIT operationFailed(*error);
        };

        if (actionId == QStringLiteral("calendar_event_accept") ||
            actionId == QStringLiteral("calendar_event_tentative") ||
            actionId == QStringLiteral("calendar_event_decline"))
        {
            const auto status =
                actionId == QStringLiteral("calendar_event_accept")      ? std::string{"accepted"}
                : actionId == QStringLiteral("calendar_event_tentative") ? std::string{"tentative"}
                                                                         : std::string{"declined"};
            auto task = m_calendarCommandPort.respondToCalendarEvent(
                account->ownerAccountId, {.accountId = account->accountId,
                                          .eventId = foundEvent->id,
                                          .participationStatus = status,
                                          .ifInState = std::nullopt,
                                          .materialization = materialization});
            QCoro::connect(std::move(task), &widget, reportResult);
            return;
        }

        if (actionId == QStringLiteral("calendar_event_copy_details"))
        {
            const auto start = QDateTime::fromString(
                QString::fromStdString(selectedEvent.start.value), Qt::ISODate);
            QStringList details{
                selectedEvent.title.empty() ? i18n("Untitled event")
                                            : QString::fromStdString(selectedEvent.title),
                selectedEvent.showWithoutTime
                    ? QLocale{}.toString(start.date(), QLocale::LongFormat)
                    : i18nc("calendar event copied date and time", "%1 at %2",
                            QLocale{}.toString(start.date(), QLocale::LongFormat),
                            QLocale{}.toString(start.time(), QLocale::ShortFormat)),
            };
            if (selectedEvent.location && !selectedEvent.location->empty())
                details.push_back(
                    i18n("Location: %1", QString::fromStdString(*selectedEvent.location)));
            if (selectedEvent.organizerCalendarAddress ||
                std::ranges::any_of(selectedEvent.attendees,
                                    [](const auto& attendee) { return attendee.isOwner; }))
                details.push_back(i18n("Organizer: %1", invitationOrganizer(selectedEvent)));
            QStringList attendees;
            for (const auto& attendee : selectedEvent.attendees)
                if (attendee.isAttendee && !attendee.isOwner)
                    attendees.push_back(participantLabel(attendee));
            if (!attendees.isEmpty())
                details.push_back(i18n("Attendees: %1", attendees.join(QStringLiteral(", "))));
            if (selectedEvent.description && !selectedEvent.description->empty())
                details.push_back(QStringLiteral("\n") +
                                  QString::fromStdString(*selectedEvent.description));
            QGuiApplication::clipboard()->setText(details.join(QLatin1Char('\n')));
            return;
        }

        if (actionId == QStringLiteral("calendar_event_move"))
        {
            if (targetCalendarId.isEmpty())
                return;
            if (!recurrenceId.isEmpty() &&
                QMessageBox::question(
                    &widget, i18n("Move recurring event"),
                    i18n("Move the entire recurring series to the selected calendar?")) !=
                    QMessageBox::Yes)
                return;
            auto moved = *foundEvent;
            moved.calendarIds.clear();
            moved.calendarIds.emplace(targetCalendarId.toStdString(), true);
            auto task = m_calendarCommandPort.updateCalendarEvent(
                account->ownerAccountId, {.accountId = account->accountId,
                                          .event = std::move(moved),
                                          .operationGroupId = std::nullopt,
                                          .ifInState = std::nullopt,
                                          .materialization = materialization});
            QCoro::connect(std::move(task), &widget, reportResult);
            return;
        }

        if (actionId == QStringLiteral("calendar_event_delete"))
        {
            bool occurrenceOnly = false;
            if (!recurrenceId.isEmpty())
            {
                QMessageBox prompt{&widget};
                prompt.setWindowTitle(i18n("Delete recurring event"));
                prompt.setText(i18n("Delete only this occurrence or the entire series?"));
                auto* occurrenceButton =
                    prompt.addButton(i18n("This occurrence"), QMessageBox::AcceptRole);
                auto* seriesButton =
                    prompt.addButton(i18n("Entire series"), QMessageBox::DestructiveRole);
                prompt.addButton(QMessageBox::Cancel);
                prompt.exec();
                if (prompt.clickedButton() == occurrenceButton)
                    occurrenceOnly = true;
                else if (prompt.clickedButton() != seriesButton)
                    return;
            }
            else if (QMessageBox::question(&widget, i18n("Delete event"),
                                           i18n("Delete this event?")) != QMessageBox::Yes)
                return;

            if (occurrenceOnly)
            {
                auto updated = javelin::jmap::calendar::excludeOccurrence(
                    *foundEvent, {.value = recurrenceId.toStdString()});
                auto task = m_calendarCommandPort.updateCalendarEvent(
                    account->ownerAccountId, {.accountId = account->accountId,
                                              .event = std::move(updated),
                                              .operationGroupId = std::nullopt,
                                              .ifInState = std::nullopt,
                                              .materialization = materialization});
                QCoro::connect(std::move(task), &widget, reportResult);
            }
            else
            {
                std::vector<std::string> calendarIds;
                for (const auto& [calendarId, present] : foundEvent->calendarIds)
                    if (present)
                        calendarIds.push_back(calendarId);
                auto task = m_calendarCommandPort.deleteCalendarEvent(
                    account->ownerAccountId, {.accountId = account->accountId,
                                              .eventId = foundEvent->id,
                                              .calendarIds = std::move(calendarIds),
                                              .operationGroupId = std::nullopt,
                                              .ifInState = std::nullopt});
                QCoro::connect(std::move(task), &widget, reportResult);
            }
            return;
        }

        if (actionId != QStringLiteral("calendar_event_duplicate"))
            return;
        std::vector<javelin::jmap::calendar::Calendar> writableCalendars;
        std::ranges::copy_if(*calendars, std::back_inserter(writableCalendars),
                             [](const auto& calendar)
                             {
                                 return calendar.isSubscribed && (calendar.myRights.mayWriteAll ||
                                                                  calendar.myRights.mayWriteOwn);
                             });
        if (writableCalendars.empty())
            return;
        auto duplicate = selectedEvent;
        const auto currentDestination = std::ranges::find_if(
            writableCalendars,
            [&duplicate](const auto& calendar)
            {
                const auto membership = duplicate.calendarIds.find(calendar.id);
                return membership != duplicate.calendarIds.end() && membership->second;
            });
        const auto defaultDestination =
            currentDestination != writableCalendars.end()
                ? currentDestination
                : std::ranges::find(writableCalendars, true,
                                    &javelin::jmap::calendar::Calendar::isDefault);
        const auto destination = defaultDestination != writableCalendars.end()
                                     ? defaultDestination
                                     : writableCalendars.begin();
        duplicate.id.clear();
        duplicate.baseEventId.reset();
        duplicate.recurrenceId.reset();
        duplicate.uid.clear();
        duplicate.isOrigin = true;
        if (!canEditEvent(*foundEvent, configuredAddress))
        {
            duplicate.organizerCalendarAddress.reset();
            duplicate.attendees.clear();
        }
        duplicate.calendarIds.clear();
        duplicate.calendarIds.emplace(destination->id, true);
        if (!recurrenceId.isEmpty())
        {
            duplicate.recurrenceRule.reset();
            duplicate.recurrenceOverrides.clear();
        }
        auto* dialog = new javelin::gui::calendar::EventDialog(writableCalendars, &widget);
        dialog->setAttribute(Qt::WA_DeleteOnClose, false);
        dialog->setEvent(duplicate);
        if (dialog->exec() != QDialog::Accepted)
        {
            dialog->deleteLater();
            return;
        }
        duplicate = dialog->eventDocument();
        const bool duplicateRequiresMaterialization =
            duplicate.recurrenceRule.has_value() || !duplicate.recurrenceOverrides.empty();
        auto task = m_calendarCommandPort.createCalendarEvent(
            account->ownerAccountId,
            {.accountId = duplicate.accountId,
             .event = std::move(duplicate),
             .operationGroupId = std::nullopt,
             .ifInState = std::nullopt,
             .materialization =
                 duplicateRequiresMaterialization
                     ? std::optional{javelin::jmap::calendar::CalendarRangeMaterialization{
                           .interval = interval,
                           .displayTimeZone = timeZone,
                       }}
                     : std::nullopt});
        QCoro::connect(std::move(task), dialog,
                       [dialog](javelin::jmap::calendar::CalendarMutationResult result)
                       {
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&result))
                           {
                               dialog->showMutationError(error->message);
                               dialog->show();
                               return;
                           }
                           dialog->deleteLater();
                       });
    }

    void CalendarTabController::applicationPaletteChanged()
    {
        for (auto& tab : m_tabs)
        {
            if (auto* widget = widgetForTab(&tab); widget != nullptr)
            {
                widget->applicationPaletteChanged();
            }
        }
    }

    void CalendarTabController::open(const std::optional<QDate> displayedMonth)
    {
        for (std::size_t index = 0; index < m_tabs.size(); ++index)
        {
            if (auto* widget = widgetForTab(&m_tabs[index]); widget != nullptr)
            {
                if (displayedMonth.has_value() && displayedMonth->isValid())
                    widget->setDisplayedMonth(*displayedMonth);
                Q_EMIT tabReady(static_cast<int>(index));
                return;
            }
        }

        const auto accountsResult = m_calendarReader.accounts();
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&accountsResult))
        {
            Q_EMIT operationFailed(*error);
            return;
        }
        const auto* accounts =
            std::get_if<std::vector<javelin::jmap::cache::CalendarAccount>>(&accountsResult);
        if (accounts == nullptr || accounts->empty())
        {
            Q_EMIT statusMessage(
                i18n("None of the configured servers support JMAP Calendars draft-26."), 10000);
            return;
        }

        auto* widget = new javelin::gui::calendar::MonthCalendarWidget(m_settings, &m_contentStack);
        std::vector<javelin::gui::calendar::CalendarAccountDisplay> accountDisplays;
        accountDisplays.reserve(accounts->size());
        for (const auto& account : *accounts)
            accountDisplays.push_back(
                {.id = account.accountId, .name = calendarAccountLabel(m_settings, account)});
        widget->setCalendarAccounts(std::move(accountDisplays));
        const auto loadVisible =
            [this, widget, accounts = *accounts](const QDate& start, const QDate& end)
        {
            std::vector<javelin::gui::calendar::MonthEvent> displayEvents;
            std::vector<javelin::gui::calendar::CalendarDisplay> calendarDisplays;
            const javelin::jmap::calendar::VisibleInterval interval{
                .start = {.value = start.toString(Qt::ISODate).toStdString() + "T00:00:00"},
                .end = {.value = end.toString(Qt::ISODate).toStdString() + "T00:00:00"}};
            const javelin::jmap::calendar::TimeZoneId timeZone{
                .value = QTimeZone::systemTimeZoneId().toStdString()};
            for (const auto& account : accounts)
            {
                const auto listed = m_calendarReader.calendars(account.accountId);
                const auto* calendars =
                    std::get_if<std::vector<javelin::jmap::calendar::Calendar>>(&listed);
                const auto loaded =
                    m_calendarReader.loadCached(account.accountId, interval, timeZone);
                const auto* window =
                    std::get_if<std::optional<javelin::jmap::cache::CalendarWindow>>(&loaded);
                auto displayAccount = account;
                displayAccount.name = calendarAccountLabel(m_settings, account).toStdString();
                auto presentation = javelin::gui::calendar::buildCalendarAccountPresentation(
                    displayAccount,
                    calendars != nullptr ? *calendars
                                         : std::vector<javelin::jmap::calendar::Calendar>{},
                    window != nullptr ? *window
                                      : std::optional<javelin::jmap::cache::CalendarWindow>{},
                    widget->palette().color(QPalette::Highlight));
                calendarDisplays.insert(calendarDisplays.end(),
                                        std::make_move_iterator(presentation.calendars.begin()),
                                        std::make_move_iterator(presentation.calendars.end()));
                displayEvents.insert(displayEvents.end(),
                                     std::make_move_iterator(presentation.events.begin()),
                                     std::make_move_iterator(presentation.events.end()));
            }
            widget->setCalendars(std::move(calendarDisplays));
            widget->setEvents(std::move(displayEvents));
        };
        connect(widget, &javelin::gui::calendar::MonthCalendarWidget::visibleIntervalChanged,
                widget, loadVisible);
        const auto agendaEvents = [this, widget, accounts = *accounts](const QDate& date)
        { return buildDayAgendaEvents(m_settings, m_calendarReader, *widget, accounts, date); };
        connect(
            widget, &javelin::gui::calendar::MonthCalendarWidget::dayAgendaRequested, widget,
            [this, widget, accounts = *accounts,
             agendaEvents](const QDate& date, const QString& accountId, const QString& eventId,
                           const QString& recurrenceId)
            {
                auto* dialog = new javelin::gui::calendar::DayAgendaDialog(widget);
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                const auto selected = eventId.isEmpty()
                                          ? std::nullopt
                                          : std::optional{javelin::gui::calendar::DayAgendaEventKey{
                                                .accountId = accountId,
                                                .eventId = eventId,
                                                .recurrenceId = recurrenceId,
                                            }};
                dialog->setDay(date, agendaEvents(date), selected);
                connect(dialog, &javelin::gui::calendar::DayAgendaDialog::dayChanged, dialog,
                        [widget, dialog, agendaEvents](const QDate& selectedDate)
                        {
                            widget->setSelectedDateFromAgenda(selectedDate);
                            dialog->setDay(selectedDate, agendaEvents(selectedDate));
                        });
                connect(dialog, &javelin::gui::calendar::DayAgendaDialog::newEventRequested, widget,
                        [widget](const QDateTime& start, const QDateTime& end)
                        { Q_EMIT widget->emptyTimeActivated(start, end); });
                connect(dialog, &javelin::gui::calendar::DayAgendaDialog::editRequested, widget,
                        [widget](const QString& selectedAccountId, const QString& selectedEventId,
                                 const QString& selectedRecurrenceId)
                        {
                            Q_EMIT widget->eventActivated(selectedAccountId, selectedEventId,
                                                          selectedRecurrenceId);
                        });
                connect(
                    dialog, &javelin::gui::calendar::DayAgendaDialog::eventContextMenuRequested,
                    widget,
                    [widget](const QPoint& globalPosition, const QString& selectedAccountId,
                             const QString& selectedEventId, const QString& selectedRecurrenceId)
                    {
                        Q_EMIT widget->eventContextMenuRequested(globalPosition, selectedAccountId,
                                                                 selectedEventId,
                                                                 selectedRecurrenceId);
                    });
                connect(&m_calendarCommandPort,
                        &javelin::app::CalendarCommandPort::calendarCacheCommitted, dialog,
                        [dialog, accounts,
                         agendaEvents](const javelin::app::CalendarCacheChange& change)
                        {
                            const auto owner = change.ownerAccountId.toStdString();
                            if (std::ranges::none_of(accounts, [&owner](const auto& account)
                                                     { return account.ownerAccountId == owner; }))
                                return;
                            QTimer::singleShot(0, dialog,
                                               [dialog, agendaEvents]
                                               {
                                                   dialog->setDay(dialog->date(),
                                                                  agendaEvents(dialog->date()),
                                                                  dialog->selectedEvent());
                                               });
                        });
                dialog->open();
            });
        connect(widget, &javelin::gui::calendar::MonthCalendarWidget::calendarSubscriptionChanged,
                widget,
                [this, accounts = *accounts, loadVisible, widget](const QString& displayId,
                                                                  const bool subscribed)
                {
                    const auto separator = displayId.indexOf(QLatin1Char('\n'));
                    if (separator <= 0 || separator == displayId.size() - 1)
                        return;
                    const auto accountId = displayId.first(separator).toStdString();
                    const auto account = std::ranges::find(
                        accounts, accountId, &javelin::jmap::cache::CalendarAccount::accountId);
                    if (account == accounts.end())
                        return;
                    auto task = m_calendarCommandPort.setCalendarSubscribed(
                        account->ownerAccountId, accountId,
                        displayId.sliced(separator + 1).toStdString(), subscribed);
                    QCoro::connect(std::move(task), widget,
                                   [this, loadVisible,
                                    widget](javelin::jmap::calendar::CalendarMutationResult result)
                                   {
                                       if (const auto* error =
                                               std::get_if<javelin::jmap::OperationError>(&result))
                                       {
                                           Q_EMIT operationFailed(*error);
                                           loadVisible(widget->visibleStart(),
                                                       widget->visibleEnd());
                                       }
                                   });
                });
        connect(
            widget, &javelin::gui::calendar::MonthCalendarWidget::defaultCalendarChanged, widget,
            [this, accounts = *accounts, loadVisible, widget](const QString& displayId)
            {
                const auto separator = displayId.indexOf(QLatin1Char('\n'));
                if (separator <= 0 || separator == displayId.size() - 1)
                    return;
                const auto accountId = displayId.first(separator).toStdString();
                const auto account = std::ranges::find(
                    accounts, accountId, &javelin::jmap::cache::CalendarAccount::accountId);
                if (account == accounts.end())
                    return;
                auto task = m_calendarCommandPort.setDefaultCalendar(
                    account->ownerAccountId, accountId,
                    displayId.sliced(separator + 1).toStdString());
                QCoro::connect(std::move(task), widget,
                               [this, loadVisible,
                                widget](javelin::jmap::calendar::CalendarMutationResult result)
                               {
                                   if (const auto* error =
                                           std::get_if<javelin::jmap::OperationError>(&result))
                                   {
                                       Q_EMIT operationFailed(*error);
                                       loadVisible(widget->visibleStart(), widget->visibleEnd());
                                   }
                               });
            });
        connect(
            widget, &javelin::gui::calendar::MonthCalendarWidget::calendarCreationRequested, widget,
            [this, accounts = *accounts, widget](const QString& accountId, const QString& name,
                                                 const QString& color)
            {
                const auto account =
                    std::ranges::find(accounts, accountId.toStdString(),
                                      &javelin::jmap::cache::CalendarAccount::accountId);
                if (account == accounts.end())
                    return;
                auto task = m_calendarCommandPort.createCalendar(
                    account->ownerAccountId,
                    {.accountId = account->accountId,
                     .name = name.toStdString(),
                     .color = color.isEmpty() ? std::nullopt : std::optional{color.toStdString()}});
                QCoro::connect(std::move(task), widget,
                               [this](javelin::jmap::calendar::CalendarMutationResult result)
                               {
                                   if (const auto* error =
                                           std::get_if<javelin::jmap::OperationError>(&result))
                                       Q_EMIT operationFailed(*error);
                               });
            });
        connect(widget, &javelin::gui::calendar::MonthCalendarWidget::calendarDeletionRequested,
                widget,
                [this, accounts = *accounts, widget](const QString& displayId)
                {
                    const auto separator = displayId.indexOf(QLatin1Char('\n'));
                    if (separator <= 0 || separator == displayId.size() - 1)
                        return;
                    const auto accountId = displayId.first(separator).toStdString();
                    const auto account = std::ranges::find(
                        accounts, accountId, &javelin::jmap::cache::CalendarAccount::accountId);
                    if (account == accounts.end())
                        return;
                    auto task = m_calendarCommandPort.deleteCalendar(
                        account->ownerAccountId,
                        {.accountId = accountId,
                         .calendarId = displayId.sliced(separator + 1).toStdString(),
                         .removeEvents = true});
                    QCoro::connect(std::move(task), widget,
                                   [this](javelin::jmap::calendar::CalendarMutationResult result)
                                   {
                                       if (const auto* error =
                                               std::get_if<javelin::jmap::OperationError>(&result))
                                           Q_EMIT operationFailed(*error);
                                   });
                });
        connect(&m_calendarCommandPort, &javelin::app::CalendarCommandPort::calendarCacheCommitted,
                widget,
                [widget, accounts = *accounts,
                 loadVisible](const javelin::app::CalendarCacheChange& change)
                {
                    const auto owner = change.ownerAccountId.toStdString();
                    if (std::ranges::none_of(accounts, [&owner](const auto& account)
                                             { return account.ownerAccountId == owner; }))
                        return;
                    loadVisible(widget->visibleStart(), widget->visibleEnd());
                });
        loadVisible(widget->visibleStart(), widget->visibleEnd());
        const auto refreshVisible =
            [this, widget, accounts = *accounts](const QDate& start, const QDate& end)
        {
            const javelin::jmap::calendar::VisibleInterval interval{
                .start = {.value = start.toString(Qt::ISODate).toStdString() + "T00:00:00"},
                .end = {.value = end.toString(Qt::ISODate).toStdString() + "T00:00:00"}};
            const javelin::jmap::calendar::TimeZoneId timeZone{
                .value = QTimeZone::systemTimeZoneId().toStdString()};
            std::unordered_set<std::string> owners;
            for (const auto& account : accounts)
            {
                if (!owners.insert(account.ownerAccountId).second)
                    continue;
                auto task = m_calendarCommandPort.requestCalendarRange(account.ownerAccountId,
                                                                       interval, timeZone);
                QCoro::connect(std::move(task), widget,
                               [this](javelin::jmap::calendar::CalendarRefreshResult result)
                               {
                                   if (const auto* error =
                                           std::get_if<javelin::jmap::OperationError>(&result))
                                       Q_EMIT operationFailed(*error);
                               });
            }
        };
        connect(widget, &javelin::gui::calendar::MonthCalendarWidget::visibleIntervalChanged,
                widget, refreshVisible);
        connect(
            widget, &javelin::gui::calendar::MonthCalendarWidget::emptyTimeActivated, widget,
            [this, widget, accounts = *accounts, refreshVisible](const QDateTime& start,
                                                                 const QDateTime& end)
            {
                std::vector<javelin::jmap::calendar::Calendar> choices;
                std::optional<std::size_t> destinationIndex;
                for (const auto& account : accounts)
                {
                    const auto calendarsResult = m_calendarReader.calendars(account.accountId);
                    const auto* calendars =
                        std::get_if<std::vector<javelin::jmap::calendar::Calendar>>(
                            &calendarsResult);
                    if (calendars == nullptr)
                        continue;
                    for (const auto& calendar : *calendars)
                    {
                        if (!calendar.isSubscribed)
                            continue;
                        auto choice = calendar;
                        if (accounts.size() > 1)
                            choice.name += QStringLiteral(" — %1")
                                               .arg(calendarAccountLabel(m_settings, account))
                                               .toStdString();
                        const auto writable =
                            choice.myRights.mayWriteAll || choice.myRights.mayWriteOwn;
                        if (writable && (!destinationIndex.has_value() || choice.isDefault))
                            destinationIndex = choices.size();
                        choices.push_back(std::move(choice));
                    }
                }
                if (!destinationIndex.has_value())
                {
                    Q_EMIT statusMessage(i18n("No writable calendar is available."), 5000);
                    return;
                }
                const auto& destination = choices[*destinationIndex];
                auto* dialog = new javelin::gui::calendar::EventDialog(choices, widget);
                dialog->setAttribute(Qt::WA_DeleteOnClose, false);
                dialog->setEvent(javelin::jmap::calendar::CalendarEvent{
                    .accountId = destination.accountId,
                    .id = {},
                    .baseEventId = std::nullopt,
                    .recurrenceId = std::nullopt,
                    .uid = {},
                    .calendarIds = {{destination.id, true}},
                    .title = {},
                    .description = std::nullopt,
                    .location = std::nullopt,
                    .start = {.value = start.toString(Qt::ISODate).toStdString()},
                    .duration = {.value =
                                     QStringLiteral("PT%1S").arg(start.secsTo(end)).toStdString()},
                    .timeZone =
                        javelin::jmap::calendar::TimeZoneId{
                            .value = QTimeZone::systemTimeZoneId().toStdString()},
                    .showWithoutTime = false,
                    .isDraft = false,
                    .isOrigin = true,
                    .useDefaultAlerts = false,
                    .alerts = {},
                    .utcStart = std::nullopt,
                    .utcEnd = std::nullopt,
                    .recurrenceRule = std::nullopt,
                    .recurrenceOverrides = {},
                    .attendees = {}});
                if (dialog->exec() != QDialog::Accepted)
                {
                    dialog->deleteLater();
                    return;
                }
                auto event = dialog->eventDocument();
                const auto selectedAccount = std::ranges::find(
                    accounts, event.accountId, &javelin::jmap::cache::CalendarAccount::accountId);
                if (selectedAccount == accounts.end())
                {
                    dialog->showMutationError(
                        i18n("The selected calendar account is no longer available."));
                    dialog->show();
                    return;
                }
                const bool requiresRecurrenceMaterialization =
                    event.recurrenceRule.has_value() || !event.recurrenceOverrides.empty();
                const auto materialization =
                    requiresRecurrenceMaterialization
                        ? std::optional{javelin::jmap::calendar::CalendarRangeMaterialization{
                              .interval =
                                  {
                                      .start =
                                          {
                                              .value = widget->visibleStart()
                                                           .toString(Qt::ISODate)
                                                           .toStdString() +
                                                       "T00:00:00",
                                          },
                                      .end =
                                          {
                                              .value = widget->visibleEnd()
                                                           .toString(Qt::ISODate)
                                                           .toStdString() +
                                                       "T00:00:00",
                                          },
                                  },
                              .displayTimeZone =
                                  {
                                      .value = QTimeZone::systemTimeZoneId().toStdString(),
                                  },
                          }}
                        : std::nullopt;
                auto task = m_calendarCommandPort.createCalendarEvent(
                    selectedAccount->ownerAccountId, {.accountId = event.accountId,
                                                      .event = std::move(event),
                                                      .operationGroupId = std::nullopt,
                                                      .ifInState = std::nullopt,
                                                      .materialization = materialization});
                QCoro::connect(std::move(task), dialog,
                               [dialog](javelin::jmap::calendar::CalendarMutationResult result)
                               {
                                   if (const auto* error =
                                           std::get_if<javelin::jmap::OperationError>(&result))
                                   {
                                       qCWarning(logCalendarOperations).noquote()
                                           << "calendar event creation failed" << error->message;
                                       dialog->showMutationError(error->message);
                                       dialog->show();
                                       return;
                                   }
                                   dialog->deleteLater();
                               });
            });
        connect(
            widget, &javelin::gui::calendar::MonthCalendarWidget::eventActivated, widget,
            [this, widget, accounts = *accounts, refreshVisible](
                const QString& accountId, const QString& eventId, const QString& recurrenceId)
            {
                const auto account =
                    std::ranges::find(accounts, accountId.toStdString(),
                                      &javelin::jmap::cache::CalendarAccount::accountId);
                if (account == accounts.end())
                    return;
                const javelin::jmap::calendar::VisibleInterval interval{
                    .start = {.value = widget->visibleStart().toString(Qt::ISODate).toStdString() +
                                       "T00:00:00"},
                    .end = {.value = widget->visibleEnd().toString(Qt::ISODate).toStdString() +
                                     "T00:00:00"}};
                const javelin::jmap::calendar::TimeZoneId timeZone{
                    .value = QTimeZone::systemTimeZoneId().toStdString()};
                const auto loaded =
                    m_calendarReader.loadCached(account->accountId, interval, timeZone);
                const auto* window =
                    std::get_if<std::optional<javelin::jmap::cache::CalendarWindow>>(&loaded);
                if (window == nullptr || !window->has_value())
                    return;
                const auto event = std::ranges::find(window->value().events, eventId.toStdString(),
                                                     &javelin::jmap::calendar::CalendarEvent::id);
                if (event == window->value().events.end())
                    return;
                const auto calendarsResult = m_calendarReader.calendars(account->accountId);
                const auto* calendars =
                    std::get_if<std::vector<javelin::jmap::calendar::Calendar>>(&calendarsResult);
                if (calendars == nullptr)
                    return;

                auto occurrenceEvent = *event;
                if (!recurrenceId.isEmpty())
                {
                    const auto occurrence = std::ranges::find_if(
                        window->value().occurrences,
                        [&eventId, &recurrenceId](const auto& candidate)
                        {
                            return candidate.eventId == eventId.toStdString() &&
                                   candidate.recurrenceId &&
                                   candidate.recurrenceId->value == recurrenceId.toStdString();
                        });
                    if (occurrence == window->value().occurrences.end())
                    {
                        qCWarning(logCalendarOperations).noquote()
                            << "calendar occurrence is missing from the visible cache" << accountId
                            << eventId << recurrenceId;
                        Q_EMIT statusMessage(
                            i18n("This occurrence is no longer available. Refresh and try again."),
                            10000);
                        return;
                    }
                    occurrenceEvent.start = occurrence->localStart;
                    const auto occurrenceStart = QDateTime::fromString(
                        QString::fromStdString(occurrence->localStart.value), Qt::ISODate);
                    const auto occurrenceEnd = QDateTime::fromString(
                        QString::fromStdString(occurrence->localEnd.value), Qt::ISODate);
                    if (occurrence->allDay)
                        occurrenceEvent.duration.value =
                            QStringLiteral("P%1D")
                                .arg(occurrenceStart.date().daysTo(occurrenceEnd.date()))
                                .toStdString();
                    else
                        occurrenceEvent.duration.value =
                            QStringLiteral("PT%1S")
                                .arg(occurrenceStart.secsTo(occurrenceEnd))
                                .toStdString();
                    if (const auto existingOverride =
                            event->recurrenceOverrides.find(recurrenceId.toStdString());
                        existingOverride != event->recurrenceOverrides.end() &&
                        existingOverride->second.title)
                        occurrenceEvent.title = *existingOverride->second.title;
                }

                const auto configuredAddress =
                    m_settings.accountForCachedId(QString::fromStdString(account->accountId))
                        .loginEmail.toStdString();
                if (!canEditEvent(*event, configuredAddress))
                {
                    const auto response = promptInvitationResponse(
                        widget, occurrenceEvent, canRsvp(*event, *calendars, configuredAddress));
                    if (!response)
                        return;
                    const bool requiresRecurrenceMaterialization =
                        event->recurrenceRule.has_value() || !event->recurrenceOverrides.empty();
                    auto task = m_calendarCommandPort.respondToCalendarEvent(
                        account->ownerAccountId,
                        {.accountId = account->accountId,
                         .eventId = event->id,
                         .participationStatus = *response,
                         .ifInState = std::nullopt,
                         .materialization = requiresRecurrenceMaterialization
                                                ? std::optional{javelin::jmap::calendar::
                                                                    CalendarRangeMaterialization{
                                                                        .interval = interval,
                                                                        .displayTimeZone = timeZone,
                                                                    }}
                                                : std::nullopt});
                    QCoro::connect(std::move(task), widget,
                                   [this](javelin::jmap::calendar::CalendarMutationResult result)
                                   {
                                       if (const auto* error =
                                               std::get_if<javelin::jmap::OperationError>(&result))
                                           Q_EMIT operationFailed(*error);
                                   });
                    return;
                }

                enum class EditScope
                {
                    Occurrence,
                    Series,
                };
                auto editScope = EditScope::Series;
                if (!recurrenceId.isEmpty())
                {
                    QMessageBox scopePrompt{widget};
                    scopePrompt.setWindowTitle(i18n("Edit recurring event"));
                    scopePrompt.setText(
                        i18n("Do you want to edit only this occurrence or the entire series?"));
                    auto* occurrenceButton =
                        scopePrompt.addButton(i18n("This occurrence"), QMessageBox::AcceptRole);
                    auto* seriesButton =
                        scopePrompt.addButton(i18n("Entire series"), QMessageBox::ActionRole);
                    scopePrompt.addButton(QMessageBox::Cancel);
                    scopePrompt.exec();
                    if (scopePrompt.clickedButton() == occurrenceButton)
                        editScope = EditScope::Occurrence;
                    else if (scopePrompt.clickedButton() != seriesButton)
                        return;
                }

                auto editableEvent = editScope == EditScope::Occurrence ? occurrenceEvent : *event;

                std::vector<javelin::jmap::calendar::Calendar> subscribedCalendars;
                std::ranges::copy_if(*calendars, std::back_inserter(subscribedCalendars),
                                     [](const auto& calendar) { return calendar.isSubscribed; });
                auto* dialog = new javelin::gui::calendar::EventDialog(subscribedCalendars, widget);
                dialog->setAttribute(Qt::WA_DeleteOnClose, false);
                dialog->setEvent(editableEvent);
                dialog->setOccurrenceMode(editScope == EditScope::Occurrence);
                const auto baseEvent = *event;
                const auto originalCalendarIds = event->calendarIds;
                const auto dialogResult = dialog->exec();
                if (dialogResult == QDialog::Rejected)
                {
                    dialog->deleteLater();
                    return;
                }
                auto editedEvent = dialog->eventDocument();
                const auto occurrenceEdit = editScope == EditScope::Occurrence;
                if (occurrenceEdit)
                {
                    const javelin::jmap::calendar::LocalDateTime selectedRecurrence{
                        .value = recurrenceId.toStdString()};
                    editedEvent =
                        dialogResult == javelin::gui::calendar::EventDialog::DeleteRequested
                            ? javelin::jmap::calendar::excludeOccurrence(baseEvent,
                                                                         selectedRecurrence)
                            : javelin::jmap::calendar::applyOccurrenceEdit(
                                  baseEvent, selectedRecurrence, editedEvent);
                }
                const bool deletingWholeEvent =
                    dialogResult == javelin::gui::calendar::EventDialog::DeleteRequested &&
                    !occurrenceEdit;
                const bool requiresRecurrenceMaterialization =
                    !deletingWholeEvent && (editedEvent.recurrenceRule.has_value() ||
                                            !editedEvent.recurrenceOverrides.empty());
                auto task =
                    dialogResult == javelin::gui::calendar::EventDialog::DeleteRequested &&
                            !occurrenceEdit
                        ? m_calendarCommandPort.deleteCalendarEvent(
                              account->ownerAccountId,
                              {.accountId = account->accountId,
                               .eventId = editedEvent.id,
                               .calendarIds =
                                   [&originalCalendarIds]
                               {
                                   std::vector<std::string> ids;
                                   for (const auto& [calendarId, present] : originalCalendarIds)
                                   {
                                       if (present)
                                           ids.push_back(calendarId);
                                   }
                                   return ids;
                               }(),
                               .operationGroupId = std::nullopt,
                               .ifInState = std::nullopt})
                        : m_calendarCommandPort.updateCalendarEvent(
                              account->ownerAccountId,
                              {.accountId = account->accountId,
                               .event = editedEvent,
                               .operationGroupId = std::nullopt,
                               .ifInState = std::nullopt,
                               .materialization =
                                   requiresRecurrenceMaterialization
                                       ? std::optional{javelin::jmap::calendar::
                                                           CalendarRangeMaterialization{
                                                               .interval = interval,
                                                               .displayTimeZone = timeZone,
                                                           }}
                                       : std::nullopt});
                QCoro::connect(std::move(task), dialog,
                               [dialog](javelin::jmap::calendar::CalendarMutationResult result)
                               {
                                   if (const auto* error =
                                           std::get_if<javelin::jmap::OperationError>(&result))
                                   {
                                       qCWarning(logCalendarOperations).noquote()
                                           << "calendar event mutation failed" << error->message;
                                       dialog->showMutationError(error->message);
                                       dialog->show();
                                       return;
                                   }
                                   dialog->deleteLater();
                               });
            });
        connect(widget, &javelin::gui::calendar::MonthCalendarWidget::eventContextActionRequested,
                widget,
                [this, widget](const QString& actionId, const QString& accountId,
                               const QString& eventId, const QString& recurrenceId,
                               const QString& targetCalendarId)
                {
                    handleEventContextAction(*widget, actionId, accountId, eventId, recurrenceId,
                                             targetCalendarId);
                });
        connect(
            widget, &javelin::gui::calendar::MonthCalendarWidget::eventContextMenuRequested, widget,
            [this, widget](const QPoint& globalPosition, const QString& accountId,
                           const QString& eventId, const QString& recurrenceId)
            { showEventContextMenu(*widget, globalPosition, accountId, eventId, recurrenceId); });
        refreshVisible(widget->visibleStart(), widget->visibleEnd());
        m_contentStack.addWidget(widget);
        m_tabs.push_back(TabState{
            .content =
                CalendarTabState{.title = i18n("Calendar"), .widget = widget, .selection = {}}});
        const auto index = static_cast<int>(m_tabs.size() - 1);
        if (displayedMonth.has_value() && displayedMonth->isValid())
            widget->setDisplayedMonth(*displayedMonth);
        Q_EMIT tabReady(index);
    }

    void CalendarTabController::invoke(const TabState* tab, const CalendarTabCommand command)
    {
        auto* widget = widgetForTab(tab);
        if (widget == nullptr)
            return;

        switch (command)
        {
        case CalendarTabCommand::CreateEvent:
            widget->createEvent();
            break;
        case CalendarTabCommand::PreviousMonth:
            widget->showPreviousMonth();
            break;
        case CalendarTabCommand::Today:
            widget->showToday();
            break;
        case CalendarTabCommand::NextMonth:
            widget->showNextMonth();
            break;
        }
    }

    bool CalendarTabController::available(const std::optional<std::string_view> accountId) const
    {
        const auto accounts = m_calendarReader.accounts();
        const auto* values =
            std::get_if<std::vector<javelin::jmap::cache::CalendarAccount>>(&accounts);
        if (values == nullptr)
            return false;
        return !accountId.has_value() ||
               std::ranges::any_of(*values, [accountId](const auto& account)
                                   { return account.accountId == *accountId; });
    }

    bool CalendarTabController::refresh(const TabState* tab)
    {
        auto* widget = widgetForTab(tab);
        if (widget == nullptr)
            return false;
        widget->setDisplayedMonth(widget->displayedMonth());
        return true;
    }

    bool CalendarTabController::close(TabState& tab)
    {
        auto* calendarTab = std::get_if<CalendarTabState>(&tab.content);
        if (calendarTab == nullptr || calendarTab->widget == nullptr)
            return false;
        auto* widget = calendarTab->widget;
        m_contentStack.removeWidget(widget);
        widget->deleteLater();
        calendarTab->widget = nullptr;
        return true;
    }

    QWidget* CalendarTabController::contentWidgetForTab(const TabState* tab) const
    {
        return widgetForTab(tab);
    }

    QMenu* CalendarTabController::calendarMenuForTab(const TabState* tab) const
    {
        auto* widget = widgetForTab(tab);
        return widget != nullptr ? widget->calendarMenu() : nullptr;
    }

    javelin::gui::calendar::MonthCalendarWidget*
    CalendarTabController::widgetForTab(const TabState* tab) const
    {
        if (tab == nullptr)
            return nullptr;
        const auto* calendarTab = std::get_if<CalendarTabState>(&tab->content);
        return calendarTab != nullptr ? calendarTab->widget : nullptr;
    }
} // namespace javelin::gui::shell
