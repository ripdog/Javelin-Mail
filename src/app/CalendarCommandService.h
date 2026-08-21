#pragma once

#include "app/CalendarApplicationPorts.h"

namespace javelin::app
{
    class CalendarApplicationService;

    class CalendarCommandService final : public CalendarCommandPort
    {
      public:
        explicit CalendarCommandService(CalendarApplicationService& service,
                                        QObject* parent = nullptr);

        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarRefreshResult>
        requestCalendarRange(std::string ownerAccountId,
                             javelin::jmap::calendar::VisibleInterval interval,
                             javelin::jmap::calendar::TimeZoneId displayTimeZone,
                             bool forceRefresh = false) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        createCalendarEvent(std::string ownerAccountId,
                            javelin::jmap::calendar::CreateEventCommand command,
                            undo::CommandOrigin origin = undo::CommandOrigin::User) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        updateCalendarEvent(std::string ownerAccountId,
                            javelin::jmap::calendar::UpdateEventCommand command,
                            undo::CommandOrigin origin = undo::CommandOrigin::User) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        deleteCalendarEvent(std::string ownerAccountId,
                            javelin::jmap::calendar::DeleteEventCommand command,
                            undo::CommandOrigin origin = undo::CommandOrigin::User) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        respondToCalendarEvent(std::string ownerAccountId,
                               javelin::jmap::calendar::RespondToEventCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        setCalendarSubscribed(std::string ownerAccountId, std::string accountId,
                              std::string calendarId, bool subscribed) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        setDefaultCalendar(std::string ownerAccountId, std::string accountId,
                           std::string calendarId,
                           undo::CommandOrigin origin = undo::CommandOrigin::User) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        setCalendarColor(std::string ownerAccountId, std::string accountId, std::string calendarId,
                         std::optional<std::string> color) override;
        [[nodiscard]] QCoro::Task<CalendarColorBatchResult>
        setCalendarColors(std::vector<CalendarColorChange> changes) override;
        [[nodiscard]] QCoro::Task<CalendarDefaultAlertsBatchResult>
        setCalendarDefaultAlerts(std::vector<CalendarDefaultAlertsChange> changes) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        createCalendar(std::string ownerAccountId,
                       javelin::jmap::calendar::CreateCalendarCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        deleteCalendar(std::string ownerAccountId,
                       javelin::jmap::calendar::DeleteCalendarCommand command) override;
        [[nodiscard]] javelin::jmap::calendar::CalendarPreferenceResult
        setCalendarVisible(std::string accountId, std::string calendarId, bool visible,
                           undo::CommandOrigin origin = undo::CommandOrigin::User) override;

      private:
        CalendarApplicationService& m_service;
    };

} // namespace javelin::app
