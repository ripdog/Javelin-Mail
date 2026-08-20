#include "app/CalendarCommandService.h"

#include "app/CalendarApplicationService.h"

#include <KLocalizedString>

#include <unordered_map>
#include <utility>

namespace javelin::app
{
    CalendarCommandService::CalendarCommandService(CalendarApplicationService& service,
                                                   QObject* parent)
        : CalendarCommandPort(parent), m_service(service)
    {
        connect(&m_service, &CalendarApplicationService::calendarCacheCommitted, this,
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
    CalendarCommandService::setCalendarColor(std::string ownerAccountId, std::string accountId,
                                             std::string calendarId,
                                             std::optional<std::string> color)
    {
        return m_service.setCalendarColor(std::move(ownerAccountId), std::move(accountId),
                                          std::move(calendarId), std::move(color));
    }

    QCoro::Task<CalendarColorBatchResult>
    CalendarCommandService::setCalendarColors(std::vector<CalendarColorChange> changes)
    {
        CalendarColorBatchResult summary{
            .requestedCount = changes.size(),
            .appliedCount = 0,
            .failures = {},
            .error = std::nullopt,
        };
        std::optional<javelin::jmap::OperationError> firstFailure;
        std::unordered_map<std::string, javelin::jmap::OperationError> unavailableOwners;

        for (const auto& change : changes)
        {
            if (const auto unavailable = unavailableOwners.find(change.ownerAccountId);
                unavailable != unavailableOwners.end())
            {
                summary.failures.push_back({.change = change, .error = unavailable->second});
                continue;
            }

            auto result = co_await m_service.setCalendarColor(
                change.ownerAccountId, change.accountId, change.calendarId, change.color);
            if (std::holds_alternative<javelin::jmap::calendar::CommittedMutation>(result))
            {
                ++summary.appliedCount;
                continue;
            }

            const auto& error = std::get<javelin::jmap::OperationError>(result);
            summary.failures.push_back({.change = change, .error = error});
            if (!firstFailure.has_value())
                firstFailure = error;
            if (javelin::jmap::isAuthenticationError(error) ||
                javelin::jmap::isTransientError(error))
                unavailableOwners.insert_or_assign(change.ownerAccountId, error);
        }

        if (firstFailure.has_value())
        {
            auto error = *firstFailure;
            if (summary.appliedCount > 0)
            {
                error.message =
                    i18n("Changed %1 of %2 calendar colors. %3",
                         static_cast<qulonglong>(summary.appliedCount),
                         static_cast<qulonglong>(summary.requestedCount), error.message);
            }
            else if (summary.requestedCount > 1)
            {
                error.message =
                    i18n("Could not change any of %1 calendar colors. %2",
                         static_cast<qulonglong>(summary.requestedCount), error.message);
            }
            summary.error = std::move(error);
        }
        co_return summary;
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
