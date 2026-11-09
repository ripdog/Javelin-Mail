#include "gui/shell/CalendarTabController.h"

#include "gui/calendar/CalendarEventContextMenuLayout.h"
#include "gui/calendar/CalendarPresentation.h"
#include "gui/calendar/DayAgendaDialog.h"
#include "gui/calendar/EventDialog.h"
#include "gui/calendar/MonthCalendarLayout.h"
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
#include <QPointer>
#include <QPushButton>
#include <QStackedWidget>
#include <QTime>
#include <QTimeZone>
#include <QTimer>

#include <iterator>
#include <memory>
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

        [[nodiscard]] bool hasConfiguredDefaultCalendarDestination(
            const javelin::protocol::CalendarDefaultDestination& destination)
        {
            return !destination.ownerAccountId.isEmpty() && !destination.accountId.isEmpty() &&
                   !destination.calendarId.isEmpty();
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

        [[nodiscard]] bool
        canEditEventInCalendar(const javelin::jmap::calendar::CalendarEvent& event,
                               const javelin::jmap::calendar::Calendar& calendar,
                               const std::string_view configuredAddress)
        {
            return javelin::jmap::calendar::eventEditableWithRights(event, calendar.myRights,
                                                                    configuredAddress);
        }

        [[nodiscard]] bool
        canEditEvent(const javelin::jmap::calendar::CalendarEvent& event,
                     const std::vector<javelin::jmap::calendar::Calendar>& calendars,
                     const std::string_view configuredAddress)
        {
            return std::ranges::any_of(
                calendars,
                [&event, configuredAddress](const auto& calendar)
                {
                    const auto membership = event.calendarIds.find(calendar.id);
                    return membership != event.calendarIds.end() && membership->second &&
                           canEditEventInCalendar(event, calendar, configuredAddress);
                });
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

        [[nodiscard]] std::string selfCalendarAddress(
            const javelin::jmap::calendar::CalendarEvent& event,
            const std::vector<javelin::jmap::calendar::ParticipantIdentity>* identities,
            const std::string_view fallback)
        {
            if (identities != nullptr)
            {
                const auto matches = [&event](const auto& identity)
                {
                    return javelin::jmap::calendar::participantIndexForAddress(
                               event, identity.calendarAddress)
                        .has_value();
                };
                const auto preferred =
                    std::ranges::find_if(*identities, [&matches](const auto& identity)
                                         { return identity.isDefault && matches(identity); });
                if (preferred != identities->end())
                    return preferred->calendarAddress;
                const auto any = std::ranges::find_if(*identities, matches);
                if (any != identities->end())
                    return any->calendarAddress;
            }
            return std::string{fallback};
        }

        [[nodiscard]] std::string invitationScopeKey(const std::string_view accountId,
                                                     const std::string_view eventId,
                                                     const std::string_view recurrenceId = {})
        {
            std::string key{accountId};
            key.push_back('\0');
            key.append(eventId);
            key.push_back('\0');
            key.append(recurrenceId);
            return key;
        }

        [[nodiscard]] bool
        occurrenceOverridesParticipant(const javelin::jmap::calendar::CalendarEvent& event,
                                       const std::string_view recurrenceId,
                                       const std::string_view participantId)
        {
            const auto occurrence = event.recurrenceOverrides.find(std::string{recurrenceId});
            if (occurrence == event.recurrenceOverrides.end())
                return false;
            return occurrence->second.participantOverrides.contains(std::string{participantId}) ||
                   occurrence->second.participantParticipationStatus.contains(
                       std::string{participantId});
        }

        template <typename Value>
        [[nodiscard]] const Value* valueFor(const std::unordered_map<std::string, Value>& values,
                                            const std::string& key)
        {
            const auto found = values.find(key);
            return found != values.end() ? &found->second : nullptr;
        }

        [[nodiscard]] std::vector<javelin::gui::calendar::DayAgendaEvent> buildDayAgendaEvents(
            javelin::gui::settings::GuiSettings& settings,
            javelin::gui::calendar::MonthCalendarWidget& widget,
            const std::vector<javelin::jmap::cache::CalendarAccount>& accounts,
            const std::unordered_map<std::string, std::vector<javelin::jmap::calendar::Calendar>>&
                accountCalendars,
            const std::unordered_map<std::string,
                                     std::vector<javelin::jmap::calendar::ParticipantIdentity>>&
                accountIdentities,
            const std::optional<javelin::gui::shell::CalendarViewSnapshot>& view, const QDate& date)
        {
            std::vector<javelin::gui::calendar::DayAgendaEvent> result;
            const auto displayEvents = widget.eventsForDate(date);
            result.reserve(displayEvents.size());

            std::unordered_map<std::string, javelin::jmap::calendar::PendingCalendarInvitation>
                pendingInvitations;
            if (view.has_value() && view->pendingInvitations.has_value())
            {
                for (const auto& invitation : *view->pendingInvitations)
                    pendingInvitations.insert_or_assign(
                        invitationScopeKey(invitation.accountId, invitation.eventId,
                                           invitation.recurrenceId ? invitation.recurrenceId->value
                                                                   : std::string{}),
                        invitation);
            }

            for (const auto& account : accounts)
            {
                const auto* calendars = valueFor(accountCalendars, account.accountId);
                const auto* identities = valueFor(accountIdentities, account.accountId);
                const auto* window =
                    view.has_value() ? valueFor(view->windows, account.accountId) : nullptr;
                const auto configuredAddress =
                    settings.accountForCachedId(QString::fromStdString(account.accountId))
                        .loginEmail.toStdString();

                for (const auto& displayEvent : displayEvents)
                {
                    if (displayEvent.accountId != account.accountId)
                        continue;

                    const javelin::jmap::calendar::CalendarEvent* event = nullptr;
                    if (window != nullptr)
                    {
                        const auto found =
                            std::ranges::find(window->events, displayEvent.eventId,
                                              &javelin::jmap::calendar::CalendarEvent::id);
                        if (found != window->events.end())
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

                    const auto displayRecurrenceId =
                        displayEvent.recurrenceId.value_or(std::string{});
                    const auto exactPending = pendingInvitations.find(invitationScopeKey(
                        displayEvent.accountId, displayEvent.eventId, displayRecurrenceId));
                    const auto seriesPending = pendingInvitations.find(
                        invitationScopeKey(displayEvent.accountId, displayEvent.eventId));
                    const auto* pendingInvitation =
                        exactPending != pendingInvitations.end()
                            ? &exactPending->second
                            : (seriesPending != pendingInvitations.end() ? &seriesPending->second
                                                                         : nullptr);

                    std::optional<javelin::jmap::calendar::CalendarEvent> effectiveOccurrence;
                    const javelin::jmap::calendar::CalendarEvent* detailEvent = event;
                    if (event != nullptr && !displayRecurrenceId.empty())
                    {
                        effectiveOccurrence = javelin::jmap::calendar::effectiveOccurrenceEvent(
                            *event, {.value = displayRecurrenceId});
                        if (effectiveOccurrence)
                            detailEvent = &*effectiveOccurrence;
                    }
                    const auto participantAddress =
                        detailEvent != nullptr
                            ? selfCalendarAddress(*detailEvent, identities, configuredAddress)
                            : configuredAddress;
                    QString rsvpRecurrenceId;
                    if (pendingInvitation != nullptr && pendingInvitation->recurrenceId)
                    {
                        rsvpRecurrenceId =
                            QString::fromStdString(pendingInvitation->recurrenceId->value);
                    }
                    else if (event != nullptr && detailEvent != nullptr &&
                             !displayRecurrenceId.empty())
                    {
                        const auto participant =
                            javelin::jmap::calendar::participantIndexForAddress(*detailEvent,
                                                                                participantAddress);
                        if (participant &&
                            occurrenceOverridesParticipant(*event, displayRecurrenceId,
                                                           detailEvent->attendees[*participant].id))
                            rsvpRecurrenceId = QString::fromStdString(displayRecurrenceId);
                    }

                    QString organizer;
                    QStringList attendees;
                    if (detailEvent != nullptr)
                    {
                        if (detailEvent->organizerCalendarAddress ||
                            std::ranges::any_of(detailEvent->attendees, [](const auto& attendee)
                                                { return attendee.isOwner; }))
                            organizer = invitationOrganizer(*detailEvent);
                        for (const auto& attendee : detailEvent->attendees)
                        {
                            if (attendee.isAttendee && !attendee.isOwner)
                                attendees.push_back(participantLabel(attendee));
                        }
                    }

                    auto agendaEvent =
                        javelin::gui::calendar::dayAgendaEventFromMonthEvent(displayEvent);
                    if (agendaEvent.title.isEmpty())
                        agendaEvent.title = i18n("Untitled event");
                    agendaEvent.calendarName = std::move(calendarName);
                    agendaEvent.editable =
                        detailEvent != nullptr && calendar != nullptr &&
                        canEditEventInCalendar(*detailEvent, *calendar, participantAddress);
                    agendaEvent.rsvpAllowed =
                        pendingInvitation != nullptr
                            ? pendingInvitation->rsvpAllowed
                            : detailEvent != nullptr && calendars != nullptr &&
                                  canRsvp(*detailEvent, *calendars, participantAddress);
                    agendaEvent.rsvpRecurrenceId = std::move(rsvpRecurrenceId);
                    agendaEvent.participationStatus =
                        pendingInvitation != nullptr
                            ? QString::fromStdString(pendingInvitation->participationStatus)
                        : detailEvent != nullptr ? [&]()
                    {
                        const auto participant =
                            javelin::jmap::calendar::participantIndexForAddress(*detailEvent,
                                                                                participantAddress);
                        return participant
                                   ? QString::fromStdString(
                                         detailEvent->attendees[*participant].participationStatus)
                                   : QString{};
                    }()
                                                 : QString{};
                    agendaEvent.organizer = std::move(organizer);
                    agendaEvent.location = detailEvent != nullptr && detailEvent->location
                                               ? QString::fromStdString(*detailEvent->location)
                                               : QString{};
                    agendaEvent.description =
                        detailEvent != nullptr && detailEvent->description
                            ? QString::fromStdString(*detailEvent->description)
                            : QString{};
                    agendaEvent.attendees = std::move(attendees);
                    result.push_back(std::move(agendaEvent));
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
        javelin::app::CalendarReadPort& calendarReader,
        javelin::app::CalendarCommandPort& calendarCommandPort, QStackedWidget& contentStack,
        std::vector<TabState>& tabs, QObject* parent)
        : QObject(parent), m_settings(settings), m_calendarReader(calendarReader),
          m_calendarCommandPort(calendarCommandPort), m_contentStack(contentStack), m_tabs(tabs)
    {
        QCoro::connect(loadAccountSnapshot(), this, [] {});
        connect(&m_calendarCommandPort, &javelin::app::CalendarCommandPort::calendarCacheCommitted,
                this, [this](const javelin::app::CalendarCacheChange&)
                { QCoro::connect(loadAccountSnapshot(), this, [] {}); });
    }

    void CalendarTabController::configureEventDialog(javelin::gui::calendar::EventDialog& dialog)
    {
        connect(&dialog, &javelin::gui::calendar::EventDialog::defaultNotificationsChangeRequested,
                &dialog,
                [this, &dialog](
                    const QString& accountId, const QString& calendarId,
                    std::unordered_map<std::string, javelin::jmap::calendar::Alert> withTime,
                    std::unordered_map<std::string, javelin::jmap::calendar::Alert> withoutTime)
                {
                    const auto account =
                        std::ranges::find(m_calendarAccounts, accountId.toStdString(),
                                          &javelin::jmap::cache::CalendarAccount::accountId);
                    if (account == m_calendarAccounts.end())
                    {
                        dialog.completeDefaultNotificationsChange(
                            accountId, calendarId, false,
                            i18n("The selected calendar account is no longer available."));
                        return;
                    }

                    std::vector<javelin::app::CalendarDefaultAlertsChange> changes;
                    changes.push_back({
                        .ownerAccountId = account->ownerAccountId,
                        .accountId = accountId.toStdString(),
                        .calendarId = calendarId.toStdString(),
                        .withTime = std::move(withTime),
                        .withoutTime = std::move(withoutTime),
                    });
                    auto task = m_calendarCommandPort.setCalendarDefaultAlerts(std::move(changes));
                    const QPointer<javelin::gui::calendar::EventDialog> dialogGuard{&dialog};
                    QCoro::connect(
                        std::move(task), this,
                        [this, dialogGuard, accountId,
                         calendarId](javelin::app::CalendarDefaultAlertsBatchResult result)
                        {
                            if (result.error.has_value())
                            {
                                if (result.error->outcomeUnknown)
                                {
                                    if (dialogGuard != nullptr)
                                        dialogGuard->completeDefaultNotificationsChange(
                                            accountId, calendarId, true);
                                    Q_EMIT operationFailed(*result.error);
                                    return;
                                }
                                if (dialogGuard != nullptr)
                                    dialogGuard->completeDefaultNotificationsChange(
                                        accountId, calendarId, false, result.error->message);
                                else
                                    Q_EMIT operationFailed(*result.error);
                                return;
                            }
                            if (dialogGuard != nullptr)
                                dialogGuard->completeDefaultNotificationsChange(accountId,
                                                                                calendarId, true);
                        });
                });
    }

    QCoro::Task<> CalendarTabController::loadAccountSnapshot()
    {
        const QPointer<CalendarTabController> self{this};
        const auto request = ++m_requestedAccountSnapshot;
        auto accountsResult = co_await m_calendarReader.accounts();
        if (!self)
            co_return;

        std::unordered_map<std::string, std::vector<javelin::jmap::calendar::Calendar>> calendars;
        std::unordered_map<std::string, std::vector<javelin::jmap::calendar::ParticipantIdentity>>
            identities;
        if (const auto* accounts =
                std::get_if<std::vector<javelin::jmap::cache::CalendarAccount>>(&accountsResult))
        {
            for (const auto& account : *accounts)
            {
                auto listed = co_await m_calendarReader.calendars(account.accountId);
                if (!self)
                    co_return;
                if (auto* values =
                        std::get_if<std::vector<javelin::jmap::calendar::Calendar>>(&listed))
                    calendars.insert_or_assign(account.accountId, std::move(*values));
                auto participants =
                    co_await m_calendarReader.participantIdentities(account.accountId);
                if (!self)
                    co_return;
                if (auto* values =
                        std::get_if<std::vector<javelin::jmap::calendar::ParticipantIdentity>>(
                            &participants))
                    identities.insert_or_assign(account.accountId, std::move(*values));
            }
        }
        if (request <= m_appliedAccountSnapshot)
            co_return;
        m_appliedAccountSnapshot = request;

        auto* widget = calendarWidget();
        if (const auto* readError = std::get_if<javelin::jmap::OperationError>(&accountsResult))
        {
            m_accountSnapshotState = AccountSnapshotState::Failed;
            setWorkspaceState({});
            const bool requested = std::exchange(m_pendingOpen, std::nullopt).has_value();
            if (widget != nullptr || requested)
                Q_EMIT operationFailed(*readError);
            co_return;
        }

        m_accountSnapshotState = AccountSnapshotState::Loaded;
        m_calendarAccounts =
            std::get<std::vector<javelin::jmap::cache::CalendarAccount>>(std::move(accountsResult));
        m_accountCalendars = std::move(calendars);
        m_accountIdentities = std::move(identities);
        setWorkspaceState(calculateWorkspaceState());
        if (widget != nullptr)
        {
            showCalendarAccounts(*widget);
            reloadView(*widget);
        }
        if (auto pending = std::exchange(m_pendingOpen, std::nullopt))
            showCalendar(std::move(*pending));
    }

    QCoro::Task<> CalendarTabController::ensureAccountsLoaded()
    {
        if (m_accountSnapshotState == AccountSnapshotState::Unloaded)
            co_await loadAccountSnapshot();
    }

    QCoro::Task<> CalendarTabController::refreshAccountsAndRange(
        QPointer<javelin::gui::calendar::MonthCalendarWidget> widget)
    {
        const QPointer<CalendarTabController> self{this};
        co_await loadAccountSnapshot();
        if (!self || widget.isNull() || m_accountSnapshotState != AccountSnapshotState::Loaded)
            co_return;
        requestVisibleRange(*widget, widget->visibleStart(), widget->visibleEnd(), true);
    }

    void CalendarTabController::showCalendarAccounts(
        javelin::gui::calendar::MonthCalendarWidget& widget) const
    {
        std::vector<javelin::gui::calendar::CalendarAccountDisplay> displays;
        displays.reserve(m_calendarAccounts.size());
        for (const auto& account : m_calendarAccounts)
            displays.push_back(
                {.id = account.accountId, .name = calendarAccountLabel(m_settings, account)});
        widget.setCalendarAccounts(std::move(displays));
    }

    const std::vector<javelin::jmap::calendar::Calendar>*
    CalendarTabController::calendarsFor(const std::string& accountId) const
    {
        return valueFor(m_accountCalendars, accountId);
    }

    const std::vector<javelin::jmap::calendar::ParticipantIdentity>*
    CalendarTabController::identitiesFor(const std::string& accountId) const
    {
        return valueFor(m_accountIdentities, accountId);
    }

    const javelin::jmap::cache::CalendarWindow*
    CalendarTabController::windowFor(const std::string& accountId) const
    {
        return m_view.has_value() ? valueFor(m_view->windows, accountId) : nullptr;
    }

    bool CalendarTabController::viewMatches(
        const javelin::gui::calendar::MonthCalendarWidget& widget) const
    {
        return m_view.has_value() && m_view->start == widget.visibleStart() &&
               m_view->end == widget.visibleEnd();
    }

    void CalendarTabController::reloadView(javelin::gui::calendar::MonthCalendarWidget& widget)
    {
        QCoro::connect(loadView(&widget), this, [] {});
    }

    QCoro::Task<>
    CalendarTabController::loadView(QPointer<javelin::gui::calendar::MonthCalendarWidget> widget)
    {
        const QPointer<CalendarTabController> self{this};
        const auto generation = ++m_viewGeneration;
        // Only the newest load may present: earlier ones describe a range or cache state that
        // has since been superseded.
        const auto current = [&self, &widget, generation]
        { return !self.isNull() && !widget.isNull() && generation == self->m_viewGeneration; };

        CalendarViewSnapshot view{
            .start = widget->visibleStart(),
            .end = widget->visibleEnd(),
            .windows = {},
            .pendingInvitations = std::nullopt,
        };
        const javelin::jmap::calendar::VisibleInterval interval{
            .start = {.value = view.start.toString(Qt::ISODate).toStdString() + "T00:00:00"},
            .end = {.value = view.end.toString(Qt::ISODate).toStdString() + "T00:00:00"}};
        const javelin::jmap::calendar::TimeZoneId timeZone{
            .value = QTimeZone::systemTimeZoneId().toStdString()};
        const auto accounts = m_calendarAccounts;

        auto pending = co_await m_calendarReader.pendingInvitations();
        if (!current())
            co_return;
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&pending))
            Q_EMIT operationFailed(*error);
        else
            view.pendingInvitations =
                std::get<std::vector<javelin::jmap::calendar::PendingCalendarInvitation>>(
                    std::move(pending));

        for (const auto& account : accounts)
        {
            auto loaded =
                co_await m_calendarReader.loadCached(account.accountId, interval, timeZone);
            if (!current())
                co_return;
            if (auto* window =
                    std::get_if<std::optional<javelin::jmap::cache::CalendarWindow>>(&loaded);
                window != nullptr && window->has_value())
                view.windows.insert_or_assign(account.accountId, std::move(**window));
        }

        m_view = std::move(view);
        presentView(*widget);
    }

    void
    CalendarTabController::requestVisibleRange(javelin::gui::calendar::MonthCalendarWidget& widget,
                                               const QDate& start, const QDate& end,
                                               const bool forceRefresh)
    {
        const javelin::jmap::calendar::VisibleInterval interval{
            .start = {.value = start.toString(Qt::ISODate).toStdString() + "T00:00:00"},
            .end = {.value = end.toString(Qt::ISODate).toStdString() + "T00:00:00"}};
        const javelin::jmap::calendar::TimeZoneId timeZone{
            .value = QTimeZone::systemTimeZoneId().toStdString()};
        std::unordered_set<std::string> owners;
        for (const auto& account : m_calendarAccounts)
        {
            if (!owners.insert(account.ownerAccountId).second)
                continue;
            auto task = m_calendarCommandPort.requestCalendarRange(account.ownerAccountId, interval,
                                                                   timeZone, forceRefresh);
            QCoro::connect(std::move(task), &widget,
                           [this](javelin::jmap::calendar::CalendarRefreshResult result)
                           {
                               if (const auto* error =
                                       std::get_if<javelin::jmap::OperationError>(&result))
                                   Q_EMIT operationFailed(*error);
                           });
        }
    }

    void CalendarTabController::configureEventContextMenu(
        std::function<std::vector<QString>()> configuredLayout, CalendarEventContextActions actions)
    {
        m_configuredEventContextMenuLayout = std::move(configuredLayout);
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

    void
    CalendarTabController::showEventContextMenu(javelin::gui::calendar::MonthCalendarWidget& widget,
                                                QWidget& popupParent, const QPoint& globalPosition,
                                                const QString& accountId, const QString& eventId,
                                                const QString& recurrenceId)
    {
        if (!m_configuredEventContextMenuLayout || !m_eventContextActions)
            return;

        const auto account = std::ranges::find(m_calendarAccounts, accountId.toStdString(),
                                               &javelin::jmap::cache::CalendarAccount::accountId);
        if (account == m_calendarAccounts.end())
            return;
        const auto* window = windowFor(account->accountId);
        if (window == nullptr)
            return;
        const auto event = std::ranges::find(window->events, eventId.toStdString(),
                                             &javelin::jmap::calendar::CalendarEvent::id);
        if (event == window->events.end())
            return;
        const auto* calendars = calendarsFor(account->accountId);
        if (calendars == nullptr)
            return;

        const auto writable = [](const javelin::jmap::calendar::Calendar& calendar)
        { return calendar.myRights.mayWriteAll || calendar.myRights.mayWriteOwn; };
        const auto configuredAddress =
            m_settings.accountForCachedId(QString::fromStdString(account->accountId))
                .loginEmail.toStdString();
        const auto* identities = identitiesFor(account->accountId);
        std::optional<javelin::jmap::calendar::CalendarEvent> occurrenceEvent;
        const javelin::jmap::calendar::CalendarEvent* actionEvent = &*event;
        if (!recurrenceId.isEmpty())
        {
            occurrenceEvent = javelin::jmap::calendar::effectiveOccurrenceEvent(
                *event, {.value = recurrenceId.toStdString()});
            if (occurrenceEvent)
                actionEvent = &*occurrenceEvent;
        }
        const auto participantAddress =
            selfCalendarAddress(*actionEvent, identities, configuredAddress);
        const auto participant =
            javelin::jmap::calendar::participantIndexForAddress(*actionEvent, participantAddress);
        const bool responsePending =
            participant &&
            actionEvent->attendees[*participant].participationStatus == "needs-action";
        const bool editable = canEditEvent(*actionEvent, *calendars, participantAddress);
        const bool rsvp = !editable && canRsvp(*actionEvent, *calendars, participantAddress);
        const bool canDuplicate =
            std::ranges::any_of(*calendars, [&writable](const auto& calendar)
                                { return calendar.isSubscribed && writable(calendar); });

        QMenu popup{&popupParent};
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
                moveMenu = new QMenu(i18n("Move to Calendar"), &popup);
                moveMenu->setIcon(QIcon::fromTheme(QStringLiteral("mail-move")));
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

        bool hasAction = false;
        bool separatorPending = false;
        for (const auto& id : javelin::gui::calendar::visibleCalendarEventContextMenuLayout(
                 m_configuredEventContextMenuLayout(), {.editable = editable,
                                                        .duplicable = canDuplicate,
                                                        .movable = editable,
                                                        .rsvp = rsvp,
                                                        .responsePending = responsePending}))
        {
            if (id == javelin::gui::calendar::calendarEventContextMenuSeparatorId())
            {
                separatorPending = hasAction;
                continue;
            }
            auto* action = actionForId(id);
            if (action == nullptr)
                continue;
            action->setEnabled(true);
            if (separatorPending)
            {
                popup.addSeparator();
                separatorPending = false;
            }
            popup.addAction(action);
            hasAction = true;
        }
        if (hasAction)
            popup.exec(globalPosition);

        for (auto* action :
             {&m_eventContextActions->edit, &m_eventContextActions->duplicate,
              &m_eventContextActions->move, &m_eventContextActions->accept,
              &m_eventContextActions->tentative, &m_eventContextActions->decline,
              &m_eventContextActions->copyDetails, &m_eventContextActions->deleteEvent})
            action->setEnabled(false);
    }

    void CalendarTabController::handleEventContextAction(
        javelin::gui::calendar::MonthCalendarWidget& widget, const QString& actionId,
        const QString& accountId, const QString& eventId, const QString& recurrenceId,
        const QString& targetCalendarId)
    {
        if (actionId == QStringLiteral("calendar_event_edit"))
        {
            Q_EMIT widget.eventEditRequested(accountId, eventId, recurrenceId);
            return;
        }

        const auto account = std::ranges::find(m_calendarAccounts, accountId.toStdString(),
                                               &javelin::jmap::cache::CalendarAccount::accountId);
        if (account == m_calendarAccounts.end())
            return;
        const javelin::jmap::calendar::VisibleInterval interval{
            .start = {.value =
                          widget.visibleStart().toString(Qt::ISODate).toStdString() + "T00:00:00"},
            .end = {.value =
                        widget.visibleEnd().toString(Qt::ISODate).toStdString() + "T00:00:00"}};
        const javelin::jmap::calendar::TimeZoneId timeZone{
            .value = QTimeZone::systemTimeZoneId().toStdString()};
        // Copies: the modal prompts below run nested event processing, which may replace the
        // presented view snapshot.
        const auto* presentedWindow = windowFor(account->accountId);
        if (presentedWindow == nullptr)
            return;
        const auto windowSnapshot = *presentedWindow;
        const auto* window = &windowSnapshot;
        const auto foundEvent = std::ranges::find(window->events, eventId.toStdString(),
                                                  &javelin::jmap::calendar::CalendarEvent::id);
        if (foundEvent == window->events.end())
            return;
        const auto* presentedCalendars = calendarsFor(account->accountId);
        if (presentedCalendars == nullptr)
            return;
        const auto calendarSnapshot = *presentedCalendars;
        const auto* calendars = &calendarSnapshot;
        const auto configuredAddress =
            m_settings.accountForCachedId(QString::fromStdString(account->accountId))
                .loginEmail.toStdString();
        const auto* identities = identitiesFor(account->accountId);
        const auto ownerAccountId = account->ownerAccountId;
        const auto calendarAccountId = account->accountId;
        auto selectedEvent = *foundEvent;
        if (!recurrenceId.isEmpty())
        {
            const auto effective = javelin::jmap::calendar::effectiveOccurrenceEvent(
                *foundEvent, {.value = recurrenceId.toStdString()});
            if (!effective)
            {
                Q_EMIT statusMessage(
                    i18n("This occurrence is no longer available. Refresh and try again."), 10000);
                return;
            }
            selectedEvent = *effective;
            const auto occurrence = std::ranges::find_if(
                window->occurrences,
                [&eventId, &recurrenceId](const auto& candidate)
                {
                    return candidate.eventId == eventId.toStdString() && candidate.recurrenceId &&
                           candidate.recurrenceId->value == recurrenceId.toStdString();
                });
            if (occurrence == window->occurrences.end())
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
        }
        const auto participantAddress =
            selfCalendarAddress(selectedEvent, identities, configuredAddress);

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
            std::optional<javelin::jmap::calendar::LocalDateTime> responseRecurrenceId;
            if (!recurrenceId.isEmpty())
            {
                if (const auto* invitations = m_view.has_value() && m_view->pendingInvitations
                                                  ? &*m_view->pendingInvitations
                                                  : nullptr)
                {
                    const auto exact = std::ranges::find_if(
                        *invitations,
                        [&accountId, &eventId, &recurrenceId](const auto& invitation)
                        {
                            return invitation.accountId == accountId.toStdString() &&
                                   invitation.eventId == eventId.toStdString() &&
                                   invitation.recurrenceId &&
                                   invitation.recurrenceId->value == recurrenceId.toStdString();
                        });
                    if (exact != invitations->end())
                        responseRecurrenceId = *exact->recurrenceId;
                }
                if (!responseRecurrenceId)
                {
                    const auto participant = javelin::jmap::calendar::participantIndexForAddress(
                        selectedEvent, participantAddress);
                    if (participant &&
                        occurrenceOverridesParticipant(*foundEvent, recurrenceId.toStdString(),
                                                       selectedEvent.attendees[*participant].id))
                        responseRecurrenceId = javelin::jmap::calendar::LocalDateTime{
                            .value = recurrenceId.toStdString()};
                }
            }
            auto task = m_calendarCommandPort.respondToCalendarEvent(
                ownerAccountId, {.accountId = calendarAccountId,
                                 .eventId = foundEvent->id,
                                 .recurrenceId = responseRecurrenceId,
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
            if (!recurrenceId.isEmpty())
            {
                const auto targetCalendar =
                    std::ranges::find(*calendars, targetCalendarId.toStdString(),
                                      &javelin::jmap::calendar::Calendar::id);
                const QString targetName = targetCalendar == calendars->end()
                                               ? targetCalendarId
                                               : QString::fromStdString(targetCalendar->name);
                if (QMessageBox::question(
                        &widget, i18n("Move recurring event"),
                        i18n("Move the entire recurring series to “%1”?\n\n%2", targetName,
                             javelin::gui::calendar::eventConfirmationDetails(selectedEvent))) !=
                    QMessageBox::Yes)
                    return;
            }
            auto moved = *foundEvent;
            moved.calendarIds.clear();
            moved.calendarIds.emplace(targetCalendarId.toStdString(), true);
            auto task = m_calendarCommandPort.updateCalendarEvent(
                ownerAccountId, {.accountId = calendarAccountId,
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
                prompt.setInformativeText(
                    javelin::gui::calendar::eventConfirmationDetails(selectedEvent));
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
                                           i18n("Delete this event?\n\n%1",
                                                javelin::gui::calendar::eventConfirmationDetails(
                                                    selectedEvent))) != QMessageBox::Yes)
                return;

            if (occurrenceOnly)
            {
                auto updated = javelin::jmap::calendar::excludeOccurrence(
                    *foundEvent, {.value = recurrenceId.toStdString()});
                auto task = m_calendarCommandPort.updateCalendarEvent(
                    ownerAccountId, {.accountId = calendarAccountId,
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
                    ownerAccountId,
                    {.accountId = calendarAccountId,
                     .eventId = foundEvent->id,
                     .calendarIds = std::move(calendarIds),
                     .operationGroupId = std::nullopt,
                     .ifInState = std::nullopt,
                     .materialization = javelin::jmap::calendar::CalendarRangeMaterialization{
                         .interval = interval,
                         .displayTimeZone = timeZone,
                     }});
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
        if (!canEditEvent(*foundEvent, *calendars, participantAddress))
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
        configureEventDialog(*dialog);
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
            ownerAccountId,
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

    void CalendarTabController::presentView(javelin::gui::calendar::MonthCalendarWidget& widget)
    {
        if (m_view->pendingInvitations.has_value())
        {
            std::vector<javelin::gui::calendar::PendingInvitationDisplay> display;
            for (const auto& invitation : *m_view->pendingInvitations)
            {
                QDateTime displayTime;
                if (invitation.displayUtc)
                {
                    displayTime =
                        QDateTime::fromString(QString::fromStdString(invitation.displayUtc->value),
                                              Qt::ISODate)
                            .toLocalTime();
                }
                if (!displayTime.isValid())
                    displayTime = QDateTime::fromString(
                        QString::fromStdString(invitation.displayTime.value), Qt::ISODate);
                auto navigationDate =
                    displayTime.isValid() ? displayTime.date() : QDate::currentDate();
                if (invitation.recurring && !invitation.recurrenceId &&
                    navigationDate < QDate::currentDate())
                    navigationDate = QDate::currentDate();
                display.push_back(javelin::gui::calendar::PendingInvitationDisplay{
                    .accountId = QString::fromStdString(invitation.accountId),
                    .eventId = QString::fromStdString(invitation.eventId),
                    .recurrenceId =
                        invitation.displayRecurrenceId
                            ? QString::fromStdString(invitation.displayRecurrenceId->value)
                            : QString{},
                    .navigationDate = navigationDate,
                    .title = invitation.title.empty() ? i18n("Untitled event")
                                                      : QString::fromStdString(invitation.title),
                    .organizer = QString::fromStdString(invitation.organizer),
                    .displayTime = displayTime.isValid() ? displayTime
                                                         : QDateTime{navigationDate, QTime{0, 0}},
                    .allDay = invitation.allDay,
                });
            }
            widget.setPendingInvitations(std::move(display));
        }

        std::vector<javelin::gui::calendar::MonthEvent> displayEvents;
        std::vector<javelin::gui::calendar::CalendarDisplay> calendarDisplays;
        for (const auto& account : m_calendarAccounts)
        {
            const auto* calendars = calendarsFor(account.accountId);
            const auto* window = windowFor(account.accountId);
            auto displayAccount = account;
            displayAccount.name = calendarAccountLabel(m_settings, account).toStdString();
            auto presentation = javelin::gui::calendar::buildCalendarAccountPresentation(
                displayAccount,
                calendars != nullptr ? *calendars
                                     : std::vector<javelin::jmap::calendar::Calendar>{},
                window != nullptr ? std::optional{*window}
                                  : std::optional<javelin::jmap::cache::CalendarWindow>{},
                widget.palette().color(QPalette::Active, QPalette::Base));
            const auto& configuredDestination =
                m_settings.workspaceSettings().defaultCalendarDestination;
            if (hasConfiguredDefaultCalendarDestination(configuredDestination))
            {
                for (auto& calendar : presentation.calendars)
                {
                    calendar.defaultDestination =
                        configuredDestination.ownerAccountId ==
                            QString::fromStdString(calendar.ownerAccountId) &&
                        configuredDestination.accountId ==
                            QString::fromStdString(calendar.accountId) &&
                        configuredDestination.calendarId ==
                            QString::fromStdString(calendar.calendarId);
                }
            }
            calendarDisplays.insert(calendarDisplays.end(),
                                    std::make_move_iterator(presentation.calendars.begin()),
                                    std::make_move_iterator(presentation.calendars.end()));
            displayEvents.insert(displayEvents.end(),
                                 std::make_move_iterator(presentation.events.begin()),
                                 std::make_move_iterator(presentation.events.end()));
        }
        widget.setPresentation(std::move(calendarDisplays), std::move(displayEvents));

        if (m_pendingDayAgenda.has_value() && viewMatches(widget))
        {
            const auto request = *std::exchange(m_pendingDayAgenda, std::nullopt);
            Q_EMIT widget.dayAgendaRequested(request.date, request.event.accountId,
                                             request.event.eventId, request.event.recurrenceId);
        }
    }

    void CalendarTabController::open(const std::optional<QDate> displayedMonth)
    {
        showCalendar({.displayedMonth = displayedMonth, .event = std::nullopt});
    }

    javelin::gui::calendar::MonthCalendarWidget* CalendarTabController::calendarWidget() const
    {
        for (const auto& tab : m_tabs)
        {
            if (auto* widget = widgetForTab(&tab); widget != nullptr)
                return widget;
        }
        return nullptr;
    }

    void CalendarTabController::showCalendar(PendingCalendarOpen request)
    {
        for (std::size_t index = 0; index < m_tabs.size(); ++index)
        {
            if (auto* widget = widgetForTab(&m_tabs[index]); widget != nullptr)
            {
                if (request.displayedMonth.has_value() && request.displayedMonth->isValid())
                    widget->setDisplayedMonth(*request.displayedMonth);
                Q_EMIT tabReady(static_cast<int>(index));
                focusRequest(*widget, request);
                return;
            }
        }

        if (m_accountSnapshotState != AccountSnapshotState::Loaded)
        {
            // The newest request wins; it opens once the account read settles.
            m_pendingOpen = std::move(request);
            QCoro::connect(loadAccountSnapshot(), this, [] {});
            return;
        }
        createCalendarTab(std::move(request));
    }

    void CalendarTabController::focusRequest(javelin::gui::calendar::MonthCalendarWidget& widget,
                                             const PendingCalendarOpen& request)
    {
        if (!request.event.has_value())
            return;
        const auto targetDate = request.displayedMonth.value_or(QDate::currentDate());
        widget.setDisplayedMonth(targetDate);
        widget.setSelectedDateFromAgenda(targetDate);
        QTimer::singleShot(0, &widget,
                           [widget = &widget, targetDate, event = *request.event]
                           {
                               Q_EMIT widget->dayAgendaRequested(targetDate, event.accountId,
                                                                 event.eventId, event.recurrenceId);
                           });
    }

    void CalendarTabController::createCalendarTab(PendingCalendarOpen request)
    {
        if (m_calendarAccounts.empty())
        {
            Q_EMIT statusMessage(
                i18n("None of the configured servers support JMAP Calendars draft-26."), 10000);
            return;
        }
        auto* widget = new javelin::gui::calendar::MonthCalendarWidget(&m_contentStack);
        showCalendarAccounts(*widget);
        const auto loadVisible = [this, widget](const QDate&, const QDate&)
        { reloadView(*widget); };
        connect(widget, &javelin::gui::calendar::MonthCalendarWidget::visibleIntervalChanged,
                widget, loadVisible);
        const auto agendaEvents = [this, widget](const QDate& date)
        {
            return buildDayAgendaEvents(m_settings, *widget, m_calendarAccounts, m_accountCalendars,
                                        m_accountIdentities, m_view, date);
        };
        const auto agendaPresentationReady = [this, widget]
        {
            return viewMatches(*widget) &&
                   std::ranges::all_of(m_calendarAccounts, [this](const auto& account)
                                       { return windowFor(account.accountId) != nullptr; });
        };
        connect(
            widget, &javelin::gui::calendar::MonthCalendarWidget::dayAgendaRequested, widget,
            [this, widget, agendaEvents,
             agendaPresentationReady](const QDate& date, const QString& accountId,
                                      const QString& eventId, const QString& recurrenceId)
            {
                if (!viewMatches(*widget))
                {
                    // Present the agenda once the visible range has been read.
                    m_pendingDayAgenda = PendingDayAgenda{
                        .date = date,
                        .event = {.accountId = accountId,
                                  .eventId = eventId,
                                  .recurrenceId = recurrenceId},
                    };
                    return;
                }
                auto* dialog = new javelin::gui::calendar::DayAgendaDialog(widget);
                const auto pendingDate = std::make_shared<std::optional<QDate>>();
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                auto selected = eventId.isEmpty()
                                    ? std::nullopt
                                    : std::optional{javelin::gui::calendar::DayAgendaEventKey{
                                          .accountId = accountId,
                                          .eventId = eventId,
                                          .recurrenceId = recurrenceId,
                                      }};
                auto events = agendaEvents(date);
                if (selected && std::ranges::none_of(events, [&selected](const auto& event)
                                                     { return event.key == *selected; }))
                {
                    selected.reset();
                    Q_EMIT statusMessage(i18n("The calendar event is no longer available."), 10000);
                }
                dialog->setDay(date, std::move(events), selected);
                connect(dialog, &javelin::gui::calendar::DayAgendaDialog::dayChanged, dialog,
                        [widget, dialog, agendaEvents, pendingDate](const QDate& selectedDate)
                        {
                            const bool outsidePresentation =
                                selectedDate < widget->visibleStart() ||
                                selectedDate >= widget->visibleEnd();
                            if (outsidePresentation)
                                *pendingDate = selectedDate;
                            else
                                pendingDate->reset();
                            widget->setSelectedDateFromAgenda(selectedDate);
                            if (!outsidePresentation)
                                dialog->setDay(selectedDate, agendaEvents(selectedDate));
                        });
                connect(dialog, &javelin::gui::calendar::DayAgendaDialog::newEventRequested, widget,
                        [widget](const QDateTime& start, const QDateTime& end)
                        { Q_EMIT widget->emptyTimeActivated(start, end); });
                connect(dialog, &javelin::gui::calendar::DayAgendaDialog::editRequested, widget,
                        [widget](const QString& selectedAccountId, const QString& selectedEventId,
                                 const QString& selectedRecurrenceId)
                        {
                            Q_EMIT widget->eventEditRequested(selectedAccountId, selectedEventId,
                                                              selectedRecurrenceId);
                        });
                connect(
                    dialog, &javelin::gui::calendar::DayAgendaDialog::responseRequested, widget,
                    [this, widget, dialog](
                        const QString& selectedAccountId, const QString& selectedEventId,
                        const QString& selectedRecurrenceId, const QString& participationStatus)
                    {
                        const auto responseEventKey = dialog->selectedEvent();
                        if (!responseEventKey || responseEventKey->accountId != selectedAccountId ||
                            responseEventKey->eventId != selectedEventId)
                            return;

                        const auto account =
                            std::ranges::find(m_calendarAccounts, selectedAccountId.toStdString(),
                                              &javelin::jmap::cache::CalendarAccount::accountId);
                        if (account == m_calendarAccounts.end())
                        {
                            dialog->setResponseMutationPending(
                                *responseEventKey, false,
                                i18n("The calendar account is no longer available."));
                            return;
                        }
                        const javelin::jmap::calendar::VisibleInterval interval{
                            .start =
                                {.value =
                                     widget->visibleStart().toString(Qt::ISODate).toStdString() +
                                     "T00:00:00"},
                            .end = {.value =
                                        widget->visibleEnd().toString(Qt::ISODate).toStdString() +
                                        "T00:00:00"}};
                        const javelin::jmap::calendar::TimeZoneId timeZone{
                            .value = QTimeZone::systemTimeZoneId().toStdString()};
                        auto task = m_calendarCommandPort.respondToCalendarEvent(
                            account->ownerAccountId,
                            {.accountId = account->accountId,
                             .eventId = selectedEventId.toStdString(),
                             .recurrenceId =
                                 selectedRecurrenceId.isEmpty()
                                     ? std::nullopt
                                     : std::optional{javelin::jmap::calendar::LocalDateTime{
                                           .value = selectedRecurrenceId.toStdString()}},
                             .participationStatus = participationStatus.toStdString(),
                             .ifInState = std::nullopt,
                             .materialization =
                                 javelin::jmap::calendar::CalendarRangeMaterialization{
                                     .interval = interval,
                                     .displayTimeZone = timeZone,
                                 }});
                        QCoro::connect(
                            std::move(task), dialog,
                            [dialog, responseEventKey = *responseEventKey](
                                javelin::jmap::calendar::CalendarMutationResult result)
                            {
                                if (const auto* error =
                                        std::get_if<javelin::jmap::OperationError>(&result))
                                {
                                    dialog->setResponseMutationPending(responseEventKey, false,
                                                                       error->message);
                                    return;
                                }
                                dialog->setResponseMutationPending(responseEventKey, false);
                            });
                    });
                connect(dialog, &javelin::gui::calendar::DayAgendaDialog::eventContextMenuRequested,
                        widget,
                        [this, widget, dialog](
                            const QPoint& globalPosition, const QString& selectedAccountId,
                            const QString& selectedEventId, const QString& selectedRecurrenceId)
                        {
                            showEventContextMenu(*widget, *dialog, globalPosition,
                                                 selectedAccountId, selectedEventId,
                                                 selectedRecurrenceId);
                        });
                connect(
                    widget, &javelin::gui::calendar::MonthCalendarWidget::eventPresentationChanged,
                    dialog,
                    [dialog, widget, agendaEvents, agendaPresentationReady, pendingDate]
                    {
                        if (pendingDate->has_value())
                        {
                            const auto targetDate = **pendingDate;
                            if (targetDate < widget->visibleStart() ||
                                targetDate >= widget->visibleEnd() || !agendaPresentationReady())
                                return;
                            pendingDate->reset();
                            dialog->setDay(targetDate, agendaEvents(targetDate));
                            return;
                        }
                        const auto selectedKey = dialog->selectedEvent();
                        dialog->setDay(dialog->date(), agendaEvents(dialog->date()), selectedKey);
                    },
                    Qt::QueuedConnection);
                dialog->open();
            });
        connect(widget, &javelin::gui::calendar::MonthCalendarWidget::pendingInvitationActivated,
                widget,
                [this](const QString& accountId, const QString& eventId,
                       const QString& recurrenceId, const QDate& navigationDate)
                { openEvent(accountId, eventId, recurrenceId, navigationDate); });
        connect(widget, &javelin::gui::calendar::MonthCalendarWidget::calendarSubscriptionChanged,
                widget,
                [this, loadVisible, widget](javelin::gui::calendar::CalendarIdentity calendar,
                                            const bool subscribed)
                {
                    auto task = m_calendarCommandPort.setCalendarSubscribed(
                        std::move(calendar.ownerAccountId), std::move(calendar.accountId),
                        std::move(calendar.calendarId), subscribed);
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
        connect(widget, &javelin::gui::calendar::MonthCalendarWidget::defaultCalendarChanged,
                widget,
                [this, loadVisible, widget](const QString& ownerAccountId, const QString& accountId,
                                            const QString& calendarId)
                {
                    auto workspace = m_settings.workspaceSettings();
                    workspace.defaultCalendarDestination = {
                        .ownerAccountId = ownerAccountId,
                        .accountId = accountId,
                        .calendarId = calendarId,
                    };
                    if (const auto error = m_settings.updateWorkspace(std::move(workspace)))
                    {
                        Q_EMIT statusMessage(error->detail, 10000);
                    }
                    loadVisible(widget->visibleStart(), widget->visibleEnd());
                });
        connect(widget, &javelin::gui::calendar::MonthCalendarWidget::calendarColorsChanged, widget,
                [this, widget](std::vector<javelin::gui::calendar::CalendarColorEdit> edits)
                {
                    std::vector<javelin::app::CalendarColorChange> changes;
                    changes.reserve(edits.size());
                    for (auto& edit : edits)
                    {
                        changes.push_back({
                            .ownerAccountId = std::move(edit.calendar.ownerAccountId),
                            .accountId = std::move(edit.calendar.accountId),
                            .calendarId = std::move(edit.calendar.calendarId),
                            .color = std::move(edit.color),
                        });
                    }
                    auto task = m_calendarCommandPort.setCalendarColors(std::move(changes));
                    QCoro::connect(std::move(task), widget,
                                   [this](javelin::app::CalendarColorBatchResult result)
                                   {
                                       if (result.error.has_value())
                                           Q_EMIT operationFailed(*result.error);
                                   });
                });
        connect(widget, &javelin::gui::calendar::MonthCalendarWidget::calendarManagerChangesSaved,
                widget,
                [this,
                 widget](std::vector<javelin::gui::calendar::CalendarColorEdit> colorEdits,
                         std::vector<javelin::gui::calendar::CalendarDefaultAlertsEdit> alertEdits)
                {
                    std::vector<javelin::app::CalendarColorChange> colorChanges;
                    colorChanges.reserve(colorEdits.size());
                    for (auto& edit : colorEdits)
                    {
                        colorChanges.push_back({
                            .ownerAccountId = std::move(edit.calendar.ownerAccountId),
                            .accountId = std::move(edit.calendar.accountId),
                            .calendarId = std::move(edit.calendar.calendarId),
                            .color = std::move(edit.color),
                        });
                    }
                    std::vector<javelin::app::CalendarDefaultAlertsChange> alertChanges;
                    alertChanges.reserve(alertEdits.size());
                    for (auto& edit : alertEdits)
                    {
                        alertChanges.push_back({
                            .ownerAccountId = std::move(edit.calendar.ownerAccountId),
                            .accountId = std::move(edit.calendar.accountId),
                            .calendarId = std::move(edit.calendar.calendarId),
                            .withTime = std::move(edit.withTime),
                            .withoutTime = std::move(edit.withoutTime),
                        });
                    }

                    auto task = [this, colorChanges = std::move(colorChanges),
                                 alertChanges = std::move(alertChanges)]() mutable
                        -> QCoro::Task<std::optional<javelin::jmap::OperationError>>
                    {
                        std::optional<javelin::jmap::OperationError> firstError;
                        if (!colorChanges.empty())
                        {
                            auto result = co_await m_calendarCommandPort.setCalendarColors(
                                std::move(colorChanges));
                            if (result.error.has_value())
                                co_return result.error;
                        }
                        if (!alertChanges.empty())
                        {
                            auto result = co_await m_calendarCommandPort.setCalendarDefaultAlerts(
                                std::move(alertChanges));
                            if (!firstError.has_value() && result.error.has_value())
                                firstError = result.error;
                        }
                        co_return firstError;
                    }();
                    QCoro::connect(std::move(task), widget,
                                   [this](std::optional<javelin::jmap::OperationError> error)
                                   {
                                       if (error.has_value())
                                           Q_EMIT operationFailed(*error);
                                   });
                });
        connect(
            widget, &javelin::gui::calendar::MonthCalendarWidget::calendarCreationRequested, widget,
            [this, widget](const QString& accountId, const QString& name, const QString& color)
            {
                const auto account =
                    std::ranges::find(m_calendarAccounts, accountId.toStdString(),
                                      &javelin::jmap::cache::CalendarAccount::accountId);
                if (account == m_calendarAccounts.end())
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
                [this, widget](javelin::gui::calendar::CalendarIdentity calendar)
                {
                    auto task = m_calendarCommandPort.deleteCalendar(
                        std::move(calendar.ownerAccountId),
                        {.accountId = std::move(calendar.accountId),
                         .calendarId = std::move(calendar.calendarId),
                         .removeEvents = true});
                    QCoro::connect(std::move(task), widget,
                                   [this](javelin::jmap::calendar::CalendarMutationResult result)
                                   {
                                       if (const auto* error =
                                               std::get_if<javelin::jmap::OperationError>(&result))
                                           Q_EMIT operationFailed(*error);
                                   });
                });
        reloadView(*widget);
        const auto refreshVisible = [this, widget](const QDate& start, const QDate& end)
        { requestVisibleRange(*widget, start, end); };
        connect(widget, &javelin::gui::calendar::MonthCalendarWidget::visibleIntervalChanged,
                widget, refreshVisible);
        connect(
            widget, &javelin::gui::calendar::MonthCalendarWidget::emptyTimeActivated, widget,
            [this, widget](const QDateTime& start, const QDateTime& end)
            {
                std::vector<javelin::jmap::calendar::Calendar> choices;
                std::vector<javelin::gui::calendar::NewEventCalendarCandidate> candidates;
                const auto& configuredDestination =
                    m_settings.workspaceSettings().defaultCalendarDestination;
                for (const auto& account : m_calendarAccounts)
                {
                    const auto* calendars = calendarsFor(account.accountId);
                    if (calendars == nullptr)
                        continue;
                    for (const auto& calendar : *calendars)
                    {
                        if (!calendar.isSubscribed)
                            continue;
                        auto choice = calendar;
                        if (m_calendarAccounts.size() > 1)
                            choice.name += QStringLiteral(" — %1")
                                               .arg(calendarAccountLabel(m_settings, account))
                                               .toStdString();
                        const auto writable =
                            choice.myRights.mayWriteAll || choice.myRights.mayWriteOwn;
                        candidates.push_back({
                            .ownerAccountId = account.ownerAccountId,
                            .accountId = account.accountId,
                            .calendarId = choice.id,
                            .writable = writable,
                            .serverDefault = choice.isDefault,
                        });
                        choices.push_back(std::move(choice));
                    }
                }
                const auto destinationIndex =
                    javelin::gui::calendar::preferredNewEventCalendarIndex(candidates,
                                                                           configuredDestination);
                if (!destinationIndex.has_value())
                {
                    Q_EMIT statusMessage(i18n("No writable calendar is available."), 5000);
                    return;
                }
                const auto& destination = choices[*destinationIndex];
                auto* dialog = new javelin::gui::calendar::EventDialog(choices, widget);
                configureEventDialog(*dialog);
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
                const auto selectedAccount =
                    std::ranges::find(m_calendarAccounts, event.accountId,
                                      &javelin::jmap::cache::CalendarAccount::accountId);
                if (selectedAccount == m_calendarAccounts.end())
                {
                    dialog->showMutationError(
                        i18n("The selected calendar account is no longer available."));
                    dialog->show();
                    return;
                }
                const auto materialization = javelin::jmap::calendar::CalendarRangeMaterialization{
                    .interval =
                        {
                            .start =
                                {
                                    .value =
                                        widget->visibleStart().toString(Qt::ISODate).toStdString() +
                                        "T00:00:00",
                                },
                            .end =
                                {
                                    .value =
                                        widget->visibleEnd().toString(Qt::ISODate).toStdString() +
                                        "T00:00:00",
                                },
                        },
                    .displayTimeZone =
                        {
                            .value = QTimeZone::systemTimeZoneId().toStdString(),
                        },
                };
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
            widget, &javelin::gui::calendar::MonthCalendarWidget::eventEditRequested, widget,
            [this, widget](const QString& accountId, const QString& eventId,
                           const QString& recurrenceId)
            {
                const auto foundAccount =
                    std::ranges::find(m_calendarAccounts, accountId.toStdString(),
                                      &javelin::jmap::cache::CalendarAccount::accountId);
                if (foundAccount == m_calendarAccounts.end())
                    return;
                // Copies: the modal prompts below run nested event processing, which may replace
                // the account and view snapshots.
                const auto account = std::optional{*foundAccount};
                const javelin::jmap::calendar::VisibleInterval interval{
                    .start = {.value = widget->visibleStart().toString(Qt::ISODate).toStdString() +
                                       "T00:00:00"},
                    .end = {.value = widget->visibleEnd().toString(Qt::ISODate).toStdString() +
                                     "T00:00:00"}};
                const javelin::jmap::calendar::TimeZoneId timeZone{
                    .value = QTimeZone::systemTimeZoneId().toStdString()};
                const auto* presentedWindow = windowFor(account->accountId);
                if (presentedWindow == nullptr)
                    return;
                const auto windowSnapshot = *presentedWindow;
                const auto* window = &windowSnapshot;
                const auto event = std::ranges::find(window->events, eventId.toStdString(),
                                                     &javelin::jmap::calendar::CalendarEvent::id);
                if (event == window->events.end())
                    return;
                const auto* presentedCalendars = calendarsFor(account->accountId);
                if (presentedCalendars == nullptr)
                    return;
                const auto calendarSnapshot = *presentedCalendars;
                const auto* calendars = &calendarSnapshot;

                auto occurrenceEvent = *event;
                if (!recurrenceId.isEmpty())
                {
                    const auto occurrence = std::ranges::find_if(
                        window->occurrences,
                        [&eventId, &recurrenceId](const auto& candidate)
                        {
                            return candidate.eventId == eventId.toStdString() &&
                                   candidate.recurrenceId &&
                                   candidate.recurrenceId->value == recurrenceId.toStdString();
                        });
                    if (occurrence == window->occurrences.end())
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
                    scopePrompt.setInformativeText(
                        javelin::gui::calendar::eventConfirmationDetails(occurrenceEvent));
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
                configureEventDialog(*dialog);
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
                               .ifInState = std::nullopt,
                               .materialization =
                                   javelin::jmap::calendar::CalendarRangeMaterialization{
                                       .interval = interval,
                                       .displayTimeZone = timeZone,
                                   }})
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
        connect(widget, &javelin::gui::calendar::MonthCalendarWidget::eventContextMenuRequested,
                widget,
                [this, widget](const QPoint& globalPosition, const QString& accountId,
                               const QString& eventId, const QString& recurrenceId)
                {
                    showEventContextMenu(*widget, *widget, globalPosition, accountId, eventId,
                                         recurrenceId);
                });
        refreshVisible(widget->visibleStart(), widget->visibleEnd());
        m_contentStack.addWidget(widget);
        m_tabs.push_back(TabState{
            .content =
                CalendarTabState{.title = i18n("Calendar"), .widget = widget, .selection = {}}});
        const auto index = static_cast<int>(m_tabs.size() - 1);
        if (request.displayedMonth.has_value() && request.displayedMonth->isValid())
            widget->setDisplayedMonth(*request.displayedMonth);
        Q_EMIT tabReady(index);
        focusRequest(*widget, request);
    }

    void CalendarTabController::openEvent(const QString& calendarAccountId, const QString& eventId,
                                          const QString& recurrenceId, const QDate& navigationDate)
    {
        if (calendarAccountId.isEmpty() || eventId.isEmpty())
        {
            Q_EMIT statusMessage(i18n("The calendar event is no longer available."), 10000);
            return;
        }
        const auto targetDate = navigationDate.isValid() ? navigationDate : QDate::currentDate();
        showCalendar({.displayedMonth = targetDate,
                      .event = CalendarEventTarget{.accountId = calendarAccountId,
                                                   .eventId = eventId,
                                                   .recurrenceId = recurrenceId}});
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
        case CalendarTabCommand::GoToMonth:
            widget->showDatePicker();
            break;
        case CalendarTabCommand::NextMonth:
            widget->showNextMonth();
            break;
        case CalendarTabCommand::ManageCalendars:
            widget->manageCalendars();
            break;
        case CalendarTabCommand::Refresh:
            static_cast<void>(refresh(tab));
            break;
        }
    }

    void CalendarTabController::invokeWorkspace(const CalendarTabCommand command)
    {
        const auto state = workspaceState();
        if (!state.available)
            return;
        if (command == CalendarTabCommand::CreateEvent && !state.canCreateEvent)
        {
            Q_EMIT statusMessage(i18n("No writable calendar is available."), 5000);
            return;
        }

        open();
        for (auto& tab : m_tabs)
        {
            if (widgetForTab(&tab) == nullptr)
                continue;
            invoke(&tab, command);
            return;
        }
    }

    const CalendarWorkspaceState& CalendarTabController::workspaceState() const
    {
        return m_workspaceState;
    }

    CalendarWorkspaceState CalendarTabController::calculateWorkspaceState() const
    {
        if (m_accountSnapshotState != AccountSnapshotState::Loaded || m_calendarAccounts.empty())
            return {};

        bool canCreateEvent = false;
        for (const auto& account : m_calendarAccounts)
        {
            const auto* accountCalendars = calendarsFor(account.accountId);
            if (accountCalendars == nullptr)
                continue;
            if (std::ranges::any_of(*accountCalendars,
                                    [](const auto& calendar)
                                    {
                                        return calendar.isSubscribed &&
                                               (calendar.myRights.mayWriteAll ||
                                                calendar.myRights.mayWriteOwn);
                                    }))
            {
                canCreateEvent = true;
                break;
            }
        }
        return {
            .available = true,
            .canCreateEvent = canCreateEvent,
            .canManageCalendars = true,
            .canRefresh = true,
        };
    }

    void CalendarTabController::setWorkspaceState(CalendarWorkspaceState state)
    {
        if (state == m_workspaceState)
            return;
        m_workspaceState = std::move(state);
        Q_EMIT workspaceStateChanged();
    }

    bool CalendarTabController::refresh(const TabState* tab)
    {
        auto* widget = widgetForTab(tab);
        if (widget == nullptr)
            return false;
        QCoro::connect(refreshAccountsAndRange(widget), this, [] {});
        return true;
    }

    void CalendarTabController::accountsChanged()
    {
        QCoro::connect(loadAccountSnapshot(), this, [] {});
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
        ++m_viewGeneration;
        m_view.reset();
        m_pendingDayAgenda.reset();
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
