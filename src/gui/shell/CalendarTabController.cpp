#include "gui/shell/CalendarTabController.h"

#include "gui/calendar/CalendarPresentation.h"
#include "gui/calendar/EventDialog.h"
#include "gui/calendar/MonthCalendarWidget.h"
#include "jmap/calendar/CalendarEventEditing.h"
#include "jmap/calendar/CalendarReader.h"

#include <QCoroTask>

#include <QAbstractButton>
#include <QDateTime>
#include <QDebug>
#include <QLoggingCategory>
#include <QMenu>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QStackedWidget>
#include <QTime>
#include <QTimeZone>

#include <iterator>
#include <ranges>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace javelin::gui::shell
{
    Q_LOGGING_CATEGORY(logCalendarOperations, "user.operations")

    CalendarTabController::CalendarTabController(
        javelin::gui::settings::WorkspaceSettingsPort& settings,
        javelin::jmap::calendar::CalendarReader& calendarReader,
        javelin::app::CalendarCommandPort& calendarCommandPort, QStackedWidget& contentStack,
        std::vector<TabState>& tabs, QObject* parent)
        : QObject(parent), m_settings(settings), m_calendarReader(calendarReader),
          m_calendarCommandPort(calendarCommandPort), m_contentStack(contentStack), m_tabs(tabs)
    {
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
                QStringLiteral("The configured server does not support JMAP Calendars draft-26."),
                10000);
            return;
        }

        auto* widget = new javelin::gui::calendar::MonthCalendarWidget(m_settings, &m_contentStack);
        std::vector<javelin::gui::calendar::CalendarAccountDisplay> accountDisplays;
        accountDisplays.reserve(accounts->size());
        for (const auto& account : *accounts)
            accountDisplays.push_back(
                {.id = account.accountId, .name = QString::fromStdString(account.name)});
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
                auto presentation = javelin::gui::calendar::buildCalendarAccountPresentation(
                    account,
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
        connect(widget, &javelin::gui::calendar::MonthCalendarWidget::calendarVisibilityChanged,
                widget,
                [this, loadVisible, widget](const QString& displayId, const bool visible)
                {
                    const auto separator = displayId.indexOf(QLatin1Char('\n'));
                    if (separator <= 0 || separator == displayId.size() - 1)
                        return;
                    const auto result = m_calendarCommandPort.setCalendarVisible(
                        displayId.first(separator).toStdString(),
                        displayId.sliced(separator + 1).toStdString(), visible);
                    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                    {
                        Q_EMIT operationFailed(*error);
                        loadVisible(widget->visibleStart(), widget->visibleEnd());
                    }
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
            [this, widget, accounts = *accounts, refreshVisible](const QDate& date)
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
                        auto choice = calendar;
                        if (accounts.size() > 1)
                            choice.name += QStringLiteral(" — %1")
                                               .arg(QString::fromStdString(account.name))
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
                    Q_EMIT statusMessage(QStringLiteral("No writable calendar is available."),
                                         5000);
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
                    .start = {.value =
                                  QDateTime{date, QTime{9, 0}}.toString(Qt::ISODate).toStdString()},
                    .duration = {.value = "PT1H"},
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
                        QStringLiteral("The selected calendar account is no longer available."));
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

                enum class EditScope
                {
                    Occurrence,
                    Series,
                };
                auto editScope = EditScope::Series;
                if (!recurrenceId.isEmpty())
                {
                    QMessageBox scopePrompt{widget};
                    scopePrompt.setWindowTitle(QStringLiteral("Edit recurring event"));
                    scopePrompt.setText(
                        QStringLiteral("Do you want to edit only this occurrence or the entire "
                                       "series?"));
                    auto* occurrenceButton = scopePrompt.addButton(
                        QStringLiteral("This occurrence"), QMessageBox::AcceptRole);
                    auto* seriesButton = scopePrompt.addButton(QStringLiteral("Entire series"),
                                                               QMessageBox::ActionRole);
                    scopePrompt.addButton(QMessageBox::Cancel);
                    scopePrompt.exec();
                    if (scopePrompt.clickedButton() == occurrenceButton)
                        editScope = EditScope::Occurrence;
                    else if (scopePrompt.clickedButton() != seriesButton)
                        return;
                }

                auto editableEvent = *event;
                if (editScope == EditScope::Occurrence)
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
                            QStringLiteral("This occurrence is no longer available. Refresh and "
                                           "try again."),
                            10000);
                        return;
                    }
                    editableEvent.start = occurrence->localStart;
                    const auto occurrenceStart = QDateTime::fromString(
                        QString::fromStdString(occurrence->localStart.value), Qt::ISODate);
                    const auto occurrenceEnd = QDateTime::fromString(
                        QString::fromStdString(occurrence->localEnd.value), Qt::ISODate);
                    if (occurrence->allDay)
                        editableEvent.duration.value =
                            QStringLiteral("P%1D")
                                .arg(occurrenceStart.date().daysTo(occurrenceEnd.date()))
                                .toStdString();
                    else
                        editableEvent.duration.value =
                            QStringLiteral("PT%1S")
                                .arg(occurrenceStart.secsTo(occurrenceEnd))
                                .toStdString();
                    if (const auto existingOverride =
                            event->recurrenceOverrides.find(recurrenceId.toStdString());
                        existingOverride != event->recurrenceOverrides.end() &&
                        existingOverride->second.title)
                        editableEvent.title = *existingOverride->second.title;
                }

                auto* dialog = new javelin::gui::calendar::EventDialog(*calendars, widget);
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
        refreshVisible(widget->visibleStart(), widget->visibleEnd());
        m_contentStack.addWidget(widget);
        m_tabs.push_back(
            TabState{.content = CalendarTabState{.accountId = accounts->front().ownerAccountId,
                                                 .title = QStringLiteral("Calendar"),
                                                 .widget = widget,
                                                 .selection = {}}});
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
