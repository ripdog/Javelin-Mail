#pragma once

#include "app/undo/HistoryTypes.h"
#include "jmap/calendar/CalendarCommandTypes.h"

#include <QCoroTask>

namespace javelin::app::undo
{
    class CalendarHistoryPort
    {
      public:
        virtual ~CalendarHistoryPort() = default;

        [[nodiscard]] virtual javelin::jmap::calendar::AuthoritativeCalendarEventResult
        getEffectiveCalendarEvent(std::string_view accountId,
                                  const std::optional<std::string>& eventId) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::calendar::AuthoritativeCalendarEventResult>
        getAuthoritativeCalendarEvent(std::string ownerAccountId, std::string accountId,
                                      std::optional<std::string> eventId, std::string uid) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        createCalendarEvent(std::string ownerAccountId,
                            javelin::jmap::calendar::CreateEventCommand command,
                            CommandOrigin origin) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        updateCalendarEvent(std::string ownerAccountId,
                            javelin::jmap::calendar::UpdateEventCommand command,
                            CommandOrigin origin) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        deleteCalendarEvent(std::string ownerAccountId,
                            javelin::jmap::calendar::DeleteEventCommand command,
                            CommandOrigin origin) = 0;
    };
} // namespace javelin::app::undo
