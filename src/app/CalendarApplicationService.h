#pragma once

#include "app/MailApplicationTypes.h"
#include "app/undo/CalendarHistoryPort.h"
#include "app/undo/CalendarPreferencePort.h"
#include "jmap/calendar/CalendarReader.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QFuture>
#include <QObject>
#include <QPromise>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace javelin::jmap::calendar
{
    class CalendarMutationEngine;
    class CalendarProtocolClient;
    class CalendarSyncEngine;
} // namespace javelin::jmap::calendar

namespace javelin::app::undo
{
    class UndoManager;
}

namespace javelin::app
{
    class AccountRuntimeManager;
    class ApplicationErrorCoordinator;
    class WorkScheduler;

    class CalendarApplicationService final : public QObject,
                                             public javelin::app::undo::CalendarHistoryPort,
                                             public javelin::app::undo::CalendarPreferencePort
    {
        Q_OBJECT

      public:
        CalendarApplicationService(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                                   javelin::jmap::calendar::CalendarReader& calendarReader,
                                   javelin::jmap::calendar::CalendarProtocolClient& protocolClient,
                                   javelin::jmap::calendar::CalendarSyncEngine& syncEngine,
                                   javelin::jmap::calendar::CalendarMutationEngine& mutationEngine,
                                   AccountRuntimeManager& accountRuntime,
                                   ApplicationErrorCoordinator& errorCoordinator,
                                   WorkScheduler& workScheduler,
                                   javelin::app::undo::UndoManager& undoManager,
                                   QObject* parent = nullptr);

        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarRefreshResult>
        requestCalendarRange(std::string ownerAccountId,
                             javelin::jmap::calendar::VisibleInterval interval,
                             javelin::jmap::calendar::TimeZoneId displayTimeZone);
        [[nodiscard]] std::vector<std::string> calendarMetadataReadyOwners() const;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        createCalendarEvent(std::string ownerAccountId,
                            javelin::jmap::calendar::CreateEventCommand command,
                            javelin::app::undo::CommandOrigin origin =
                                javelin::app::undo::CommandOrigin::User) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        updateCalendarEvent(std::string ownerAccountId,
                            javelin::jmap::calendar::UpdateEventCommand command,
                            javelin::app::undo::CommandOrigin origin =
                                javelin::app::undo::CommandOrigin::User) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        deleteCalendarEvent(std::string ownerAccountId,
                            javelin::jmap::calendar::DeleteEventCommand command,
                            javelin::app::undo::CommandOrigin origin =
                                javelin::app::undo::CommandOrigin::User) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        respondToCalendarEvent(std::string ownerAccountId,
                               javelin::jmap::calendar::RespondToEventCommand command);
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::AuthoritativeCalendarEventResult>
        getAuthoritativeCalendarEvent(std::string ownerAccountId, std::string accountId,
                                      std::optional<std::string> eventId, std::string uid) override;
        [[nodiscard]] javelin::jmap::calendar::AuthoritativeCalendarEventResult
        getEffectiveCalendarEvent(std::string_view accountId,
                                  const std::optional<std::string>& eventId) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        setCalendarSubscribed(
            std::string ownerAccountId, std::string accountId, std::string calendarId,
            bool subscribed,
            javelin::app::undo::CommandOrigin origin = javelin::app::undo::CommandOrigin::User);
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        setDefaultCalendar(
            std::string ownerAccountId, std::string accountId, std::string calendarId,
            javelin::app::undo::CommandOrigin origin = javelin::app::undo::CommandOrigin::User);
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        setCalendarColor(std::string ownerAccountId, std::string accountId, std::string calendarId,
                         std::optional<std::string> color);
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        createCalendar(std::string ownerAccountId,
                       javelin::jmap::calendar::CreateCalendarCommand command);
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        deleteCalendar(std::string ownerAccountId,
                       javelin::jmap::calendar::DeleteCalendarCommand command);
        [[nodiscard]] javelin::jmap::calendar::CalendarPreferenceResult setCalendarVisible(
            std::string accountId, std::string calendarId, bool visible,
            javelin::app::undo::CommandOrigin origin = javelin::app::undo::CommandOrigin::User);
        [[nodiscard]] std::variant<std::optional<std::string>, javelin::jmap::OperationError>
        currentCalendarPreference(
            const javelin::app::undo::CalendarPreferenceHistory& history) const override;
        [[nodiscard]] QCoro::Task<std::optional<javelin::jmap::OperationError>>
        applyCalendarPreference(javelin::app::undo::CalendarPreferenceHistory history,
                                std::optional<std::string> value,
                                javelin::app::undo::CommandOrigin origin) override;
        void scheduleRefresh(std::string ownerAccountId);

      Q_SIGNALS:
        void calendarCacheCommitted(javelin::app::CalendarCacheChange change);
        void calendarMetadataReady(const QString& ownerAccountId);

      private:
        void scheduleMetadataRefresh(std::string ownerAccountId);
        [[nodiscard]] QCoro::Task<std::variant<bool, javelin::jmap::OperationError>>
        requestCalendarMetadata(std::string ownerAccountId);
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarRefreshResult>
        requestCalendarChanges(std::string ownerAccountId);

        struct VisibleCalendarRange
        {
            javelin::jmap::calendar::VisibleInterval interval;
            javelin::jmap::calendar::TimeZoneId displayTimeZone;
        };

        struct RangeRefreshFlight
        {
            VisibleCalendarRange range;
            QFuture<javelin::jmap::calendar::CalendarRefreshResult> future;
            std::shared_ptr<QPromise<javelin::jmap::calendar::CalendarRefreshResult>> promise;
        };

        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::calendar::CalendarReader& m_calendarReader;
        javelin::jmap::calendar::CalendarProtocolClient& m_calendarProtocolClient;
        javelin::jmap::calendar::CalendarSyncEngine& m_calendarSyncEngine;
        javelin::jmap::calendar::CalendarMutationEngine& m_calendarMutationEngine;
        AccountRuntimeManager& m_accountRuntime;
        ApplicationErrorCoordinator& m_errorCoordinator;
        WorkScheduler& m_workScheduler;
        javelin::app::undo::UndoManager& m_undoManager;
        std::unordered_map<std::string, VisibleCalendarRange> m_visibleCalendarRanges;
        std::unordered_set<std::string> m_calendarMetadataReadyOwners;
        std::unordered_set<std::string> m_calendarMetadataUsableOwners;
        std::unordered_map<std::string, QFuture<bool>> m_calendarMetadataRefreshesInFlight;
        std::unordered_map<std::string, RangeRefreshFlight> m_calendarRangeRefreshesInFlight;
        std::unordered_set<std::string> m_calendarMetadataRefreshPending;
        std::unordered_map<std::string, QFuture<bool>> m_calendarStateRefreshesInFlight;
        std::unordered_set<std::string> m_calendarStateRefreshPending;
    };

} // namespace javelin::app
