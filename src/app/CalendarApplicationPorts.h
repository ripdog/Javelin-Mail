#pragma once

#include "app/CalendarApplicationTypes.h"
#include "app/MailApplicationTypes.h"
#include "app/undo/HistoryTypes.h"
#include "jmap/calendar/CalendarCommandTypes.h"

#include <QCoroTask>

#include <QObject>

namespace javelin::app
{

    class CalendarCommandPort : public QObject
    {
        Q_OBJECT

      public:
        using QObject::QObject;
        ~CalendarCommandPort() override = default;

        [[nodiscard]] virtual QCoro::Task<javelin::jmap::calendar::CalendarRefreshResult>
        requestCalendarRange(std::string ownerAccountId,
                             javelin::jmap::calendar::VisibleInterval interval,
                             javelin::jmap::calendar::TimeZoneId displayTimeZone) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        createCalendarEvent(std::string ownerAccountId,
                            javelin::jmap::calendar::CreateEventCommand command,
                            undo::CommandOrigin origin = undo::CommandOrigin::User) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        updateCalendarEvent(std::string ownerAccountId,
                            javelin::jmap::calendar::UpdateEventCommand command,
                            undo::CommandOrigin origin = undo::CommandOrigin::User) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        deleteCalendarEvent(std::string ownerAccountId,
                            javelin::jmap::calendar::DeleteEventCommand command,
                            undo::CommandOrigin origin = undo::CommandOrigin::User) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        respondToCalendarEvent(std::string ownerAccountId,
                               javelin::jmap::calendar::RespondToEventCommand command) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        setCalendarSubscribed(std::string ownerAccountId, std::string accountId,
                              std::string calendarId, bool subscribed) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        setDefaultCalendar(std::string ownerAccountId, std::string accountId,
                           std::string calendarId,
                           undo::CommandOrigin origin = undo::CommandOrigin::User) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        setCalendarColor(std::string ownerAccountId, std::string accountId, std::string calendarId,
                         std::optional<std::string> color) = 0;
        [[nodiscard]] virtual QCoro::Task<CalendarColorBatchResult>
        setCalendarColors(std::vector<CalendarColorChange> changes) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        createCalendar(std::string ownerAccountId,
                       javelin::jmap::calendar::CreateCalendarCommand command) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        deleteCalendar(std::string ownerAccountId,
                       javelin::jmap::calendar::DeleteCalendarCommand command) = 0;
        [[nodiscard]] virtual javelin::jmap::calendar::CalendarPreferenceResult
        setCalendarVisible(std::string accountId, std::string calendarId, bool visible,
                           undo::CommandOrigin origin = undo::CommandOrigin::User) = 0;

      Q_SIGNALS:
        void calendarCacheCommitted(javelin::app::CalendarCacheChange change);
    };

} // namespace javelin::app
