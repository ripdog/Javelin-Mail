#pragma once

#include "jmap/cache/CalendarInvitationRepository.h"

#include <QObject>
#include <QTimer>

#include <QCoroTask>

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace javelin::app
{
    class AccountRuntimeManager;
    class CalendarApplicationService;
} // namespace javelin::app

namespace javelin::jmap::calendar
{
    class CalendarProtocolClient;
    class CalendarReader;
} // namespace javelin::jmap::calendar

namespace javelin::jmap::cache
{
    class DatabaseConnection;
}

namespace javelin::app
{
    class CalendarInvitationService final : public QObject
    {
        Q_OBJECT

      public:
        CalendarInvitationService(javelin::jmap::cache::DatabaseConnection& connection,
                                  javelin::jmap::calendar::CalendarProtocolClient& protocolClient,
                                  javelin::jmap::calendar::CalendarReader& reader,
                                  AccountRuntimeManager& accountRuntime,
                                  CalendarApplicationService& calendarApplication,
                                  QObject* parent = nullptr);

        void start();
        void deliveryAccepted(const QString& invitationKey);
        void deliveryFailed(const QString& invitationKey);
        void requestDispatch();

      Q_SIGNALS:
        void invitationReady(const QString& invitationKey, const QString& calendarAccountId,
                             const QString& eventId, const QString& recurrenceId,
                             const QString& navigationDate, const QString& title,
                             const QString& message);
        void invitationResolved(const QString& invitationKey);
        void pendingInvitationCacheChanged();

      private:
        struct LiveInvitation
        {
            std::string accountId;
            std::string eventId;
        };

        void scheduleOwner(std::string ownerAccountId);
        void synchronizeQueuedOwners();
        [[nodiscard]] QCoro::Task<void> synchronizeOwner(std::string ownerAccountId);
        void dispatchPending();
        void refreshPresentationState();

        javelin::jmap::cache::DatabaseConnection& m_connection;
        javelin::jmap::calendar::CalendarProtocolClient& m_protocolClient;
        javelin::jmap::calendar::CalendarReader& m_reader;
        AccountRuntimeManager& m_accountRuntime;
        CalendarApplicationService& m_calendarApplication;
        javelin::jmap::cache::CalendarInvitationRepository m_repository;
        QTimer m_syncTimer;
        QTimer m_dispatchRetryTimer;
        std::unordered_set<std::string> m_pendingOwners;
        std::unordered_set<std::string> m_runningOwners;
        std::unordered_map<std::string, LiveInvitation> m_liveInvitations;
        bool m_started = false;
    };
} // namespace javelin::app
