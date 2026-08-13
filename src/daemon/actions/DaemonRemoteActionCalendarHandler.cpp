#include "daemon/actions/DaemonRemoteActionDispatcher.h"

#include "app/CalendarApplicationPorts.h"
#include "daemon/DaemonServices.h"
#include "jmap/calendar/CalendarReader.h"

namespace javelin::app
{
    javelin::protocol::CommandReply DaemonRemoteActionDispatcher::dispatchCalendarAction(
        const javelin::protocol::CommandId& id,
        const javelin::protocol::RemoteActionCommand& command)
    {
        namespace actions = javelin::protocol::actions;
        switch (command.action.value)
        {
        case actions::CalendarReadCached::id.value:
            return dispatchDecoded<actions::CalendarReadCached>(
                id, command,
                [this, &id](const std::string& accountId,
                            const javelin::jmap::calendar::VisibleInterval& interval,
                            const javelin::jmap::calendar::TimeZoneId& timeZone)
                {
                    return acceptValue<actions::CalendarReadCached>(
                        id, m_services.calendarReader().loadCached(accountId, interval, timeZone));
                });
        case actions::CalendarReadAccounts::id.value:
            return acceptValue<actions::CalendarReadAccounts>(
                id, m_services.calendarReader().accounts());
        case actions::CalendarReadCalendars::id.value:
            return dispatchDecoded<actions::CalendarReadCalendars>(
                id, command,
                [this, &id](const std::string& accountId)
                {
                    return acceptValue<actions::CalendarReadCalendars>(
                        id, m_services.calendarReader().calendars(accountId));
                });
        case actions::CalendarRequestRange::id.value:
            return dispatchDecoded<actions::CalendarRequestRange>(
                id, command,
                [this, &id](std::string ownerAccountId,
                            javelin::jmap::calendar::VisibleInterval interval,
                            javelin::jmap::calendar::TimeZoneId timeZone)
                {
                    return launchAction<actions::CalendarRequestRange>(
                        id,
                        m_services.calendarCommandPort().requestCalendarRange(
                            std::move(ownerAccountId), std::move(interval), std::move(timeZone)));
                });
        case actions::CalendarCreateEvent::id.value:
            return dispatchDecoded<actions::CalendarCreateEvent>(
                id, command,
                [this, &id](std::string ownerAccountId,
                            javelin::jmap::calendar::CreateEventCommand eventCommand,
                            const undo::CommandOrigin origin)
                {
                    return launchAction<actions::CalendarCreateEvent>(
                        id, m_services.calendarCommandPort().createCalendarEvent(
                                std::move(ownerAccountId), std::move(eventCommand), origin));
                });
        case actions::CalendarUpdateEvent::id.value:
            return dispatchDecoded<actions::CalendarUpdateEvent>(
                id, command,
                [this, &id](std::string ownerAccountId,
                            javelin::jmap::calendar::UpdateEventCommand eventCommand,
                            const undo::CommandOrigin origin)
                {
                    return launchAction<actions::CalendarUpdateEvent>(
                        id, m_services.calendarCommandPort().updateCalendarEvent(
                                std::move(ownerAccountId), std::move(eventCommand), origin));
                });
        case actions::CalendarDeleteEvent::id.value:
            return dispatchDecoded<actions::CalendarDeleteEvent>(
                id, command,
                [this, &id](std::string ownerAccountId,
                            javelin::jmap::calendar::DeleteEventCommand eventCommand,
                            const undo::CommandOrigin origin)
                {
                    return launchAction<actions::CalendarDeleteEvent>(
                        id, m_services.calendarCommandPort().deleteCalendarEvent(
                                std::move(ownerAccountId), std::move(eventCommand), origin));
                });
        case actions::CalendarRespondEvent::id.value:
            return dispatchDecoded<actions::CalendarRespondEvent>(
                id, command,
                [this, &id](std::string ownerAccountId,
                            javelin::jmap::calendar::RespondToEventCommand eventCommand)
                {
                    return launchAction<actions::CalendarRespondEvent>(
                        id, m_services.calendarCommandPort().respondToCalendarEvent(
                                std::move(ownerAccountId), std::move(eventCommand)));
                });
        case actions::CalendarSetSubscribed::id.value:
            return dispatchDecoded<actions::CalendarSetSubscribed>(
                id, command,
                [this, &id](std::string ownerAccountId, std::string accountId,
                            std::string calendarId, const bool subscribed)
                {
                    return launchAction<actions::CalendarSetSubscribed>(
                        id, m_services.calendarCommandPort().setCalendarSubscribed(
                                std::move(ownerAccountId), std::move(accountId),
                                std::move(calendarId), subscribed));
                });
        case actions::CalendarSetDefault::id.value:
            return dispatchDecoded<actions::CalendarSetDefault>(
                id, command,
                [this, &id](std::string ownerAccountId, std::string accountId,
                            std::string calendarId, const undo::CommandOrigin origin)
                {
                    return launchAction<actions::CalendarSetDefault>(
                        id, m_services.calendarCommandPort().setDefaultCalendar(
                                std::move(ownerAccountId), std::move(accountId),
                                std::move(calendarId), origin));
                });
        case actions::CalendarCreate::id.value:
            return dispatchDecoded<actions::CalendarCreate>(
                id, command,
                [this, &id](std::string ownerAccountId,
                            javelin::jmap::calendar::CreateCalendarCommand calendarCommand)
                {
                    return launchAction<actions::CalendarCreate>(
                        id, m_services.calendarCommandPort().createCalendar(
                                std::move(ownerAccountId), std::move(calendarCommand)));
                });
        case actions::CalendarDelete::id.value:
            return dispatchDecoded<actions::CalendarDelete>(
                id, command,
                [this, &id](std::string ownerAccountId,
                            javelin::jmap::calendar::DeleteCalendarCommand calendarCommand)
                {
                    return launchAction<actions::CalendarDelete>(
                        id, m_services.calendarCommandPort().deleteCalendar(
                                std::move(ownerAccountId), std::move(calendarCommand)));
                });
        case actions::CalendarSetVisible::id.value:
            return dispatchDecoded<actions::CalendarSetVisible>(
                id, command,
                [this, &id](std::string accountId, std::string calendarId, const bool visible,
                            const undo::CommandOrigin origin)
                {
                    return acceptValue<actions::CalendarSetVisible>(
                        id, m_services.calendarCommandPort().setCalendarVisible(
                                std::move(accountId), std::move(calendarId), visible, origin));
                });
        default:
            return reject(id, QStringLiteral("The calendar action is unsupported."),
                          javelin::protocol::BoundaryErrorCode::UnsupportedOperation);
        }
    }
} // namespace javelin::app
