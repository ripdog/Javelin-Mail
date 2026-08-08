#include "app/CalendarCommandService.h"

#include "app/MailApplicationService.h"

#include <utility>

namespace javelin::app
{
    CalendarCommandService::CalendarCommandService(MailApplicationService& service, QObject* parent)
        : CalendarCommandPort(parent), m_service(service)
    {
        connect(&m_service, &MailApplicationService::calendarCacheCommitted, this,
                &CalendarCommandPort::calendarCacheCommitted);
    }

    QCoro::Task<javelin::jmap::calendar::CalendarRefreshResult>
    CalendarCommandService::requestCalendarRange(
        std::string ownerAccountId, javelin::jmap::calendar::VisibleInterval interval,
        javelin::jmap::calendar::TimeZoneId displayTimeZone)
    {
        return m_service.requestCalendarRange(std::move(ownerAccountId), std::move(interval),
                                              std::move(displayTimeZone));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    CalendarCommandService::createCalendarEvent(std::string ownerAccountId,
                                                javelin::jmap::calendar::CreateEventCommand command,
                                                const undo::CommandOrigin origin)
    {
        return m_service.createCalendarEvent(std::move(ownerAccountId), std::move(command), origin);
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    CalendarCommandService::updateCalendarEvent(std::string ownerAccountId,
                                                javelin::jmap::calendar::UpdateEventCommand command,
                                                const undo::CommandOrigin origin)
    {
        return m_service.updateCalendarEvent(std::move(ownerAccountId), std::move(command), origin);
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    CalendarCommandService::deleteCalendarEvent(std::string ownerAccountId,
                                                javelin::jmap::calendar::DeleteEventCommand command,
                                                const undo::CommandOrigin origin)
    {
        return m_service.deleteCalendarEvent(std::move(ownerAccountId), std::move(command), origin);
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    CalendarCommandService::respondToCalendarEvent(
        std::string ownerAccountId, javelin::jmap::calendar::RespondToEventCommand command)
    {
        return m_service.respondToCalendarEvent(std::move(ownerAccountId), std::move(command));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    CalendarCommandService::setCalendarSubscribed(std::string ownerAccountId, std::string accountId,
                                                  std::string calendarId, const bool subscribed)
    {
        return m_service.setCalendarSubscribed(std::move(ownerAccountId), std::move(accountId),
                                               std::move(calendarId), subscribed);
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    CalendarCommandService::setDefaultCalendar(std::string ownerAccountId, std::string accountId,
                                               std::string calendarId,
                                               const undo::CommandOrigin origin)
    {
        return m_service.setDefaultCalendar(std::move(ownerAccountId), std::move(accountId),
                                            std::move(calendarId), origin);
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    CalendarCommandService::createCalendar(std::string ownerAccountId,
                                           javelin::jmap::calendar::CreateCalendarCommand command)
    {
        return m_service.createCalendar(std::move(ownerAccountId), std::move(command));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    CalendarCommandService::deleteCalendar(std::string ownerAccountId,
                                           javelin::jmap::calendar::DeleteCalendarCommand command)
    {
        return m_service.deleteCalendar(std::move(ownerAccountId), std::move(command));
    }

    javelin::jmap::calendar::CalendarPreferenceResult
    CalendarCommandService::setCalendarVisible(std::string accountId, std::string calendarId,
                                               const bool visible, const undo::CommandOrigin origin)
    {
        return m_service.setCalendarVisible(std::move(accountId), std::move(calendarId), visible,
                                            origin);
    }

} // namespace javelin::app
