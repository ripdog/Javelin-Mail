#pragma once

#include "storage/sqlite/DatabaseConnection.h"

#include "app/AccountConnectionProvider.h"
#include "app/AccountConnectionSettings.h"
#include "app/AccountRefreshApplicationPorts.h"
#include "app/ContactApplicationPorts.h"
#include "app/LongPollService.h"
#include "app/MailApplicationPorts.h"
#include "app/MailApplicationTypes.h"
#include "app/MailboxSelectionMutation.h"
#include "app/MessageListMaterializationPort.h"
#include "app/undo/CalendarHistoryPort.h"
#include "app/undo/CalendarPreferencePort.h"
#include "app/undo/HistoryTypes.h"
#include "app/undo/MailHistoryPort.h"
#include "app/undo/SieveHistoryPort.h"
#include "jmap/calendar/CalendarService.h"
#include "jmap/query/EmailListSort.h"
#include "jmap/search/EmailSearch.h"
#include "jmap/sieve/SieveService.h"
#include "jmap/sync/MailboxInterestRegistry.h"

#include <QObject>
#include <QPointer>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace javelin::jmap::contacts
{
    class ContactService;
}

namespace javelin::jmap::cache
{
    class ContactReader;
    class ContactRepository;
    class MailboxFilterReader;
    class MailboxMessageReader;
    class MailboxReader;
    class MailTagReader;
    class MailboxStatisticsReader;
} // namespace javelin::jmap::cache

namespace javelin::app::undo
{
    class UndoManager;
}

namespace javelin::app
{
    class ApplicationErrorCoordinator;
    class MailboxMaintenanceRegistry;
    class ThreadMaterializationCoordinator;
    class WorkScheduler;

    class MailboxObservation;

    struct AccountSyncConfiguration
    {
        AccountConnectionSettings settings;
        std::string accountId;
        std::vector<std::string> mailboxIds;
        std::vector<std::string> fullSyncMailboxIds;
        std::vector<std::string> notificationMailboxIds;

        friend bool operator==(const AccountSyncConfiguration&,
                               const AccountSyncConfiguration&) = default;
    };

    class MailApplicationService final : public QObject,
                                         public MessageListMaterializationPort,
                                         public AccountConnectionProvider,
                                         public MailCacheChangePublisher,
                                         public ContactRefreshPort,
                                         public javelin::app::undo::MailHistoryPort,
                                         public javelin::app::undo::SieveHistoryPort,
                                         public javelin::app::undo::CalendarHistoryPort,
                                         public javelin::app::undo::CalendarPreferencePort
    {
        Q_OBJECT

      public:
        MailApplicationService(
            javelin::jmap::cache::DatabaseConnection& databaseConnection,
            javelin::jmap::JmapCore& jmapCore,
            javelin::jmap::api::JmapMethodTransport& methodTransport,
            QNetworkAccessManager& networkAccessManager,
            javelin::jmap::api::WebSocketFailureCooldowns& cooldowns,
            javelin::jmap::cache::AccountRepository& accountRepository,
            javelin::jmap::cache::MailboxReader& mailboxReader,
            javelin::jmap::cache::MailTagReader& mailTagReader,
            javelin::jmap::cache::MailboxStatisticsReader& mailboxStatisticsReader,
            javelin::jmap::cache::MailboxMessageReader& mailboxMessageReader,
            javelin::jmap::cache::MailboxFilterReader& mailboxFilterReader,
            javelin::jmap::cache::ContactRepository& contactRepository,
            javelin::jmap::contacts::ContactService& contactService,
            javelin::jmap::calendar::CalendarService& calendarService,
            javelin::jmap::sieve::SieveService& sieveService,
            ApplicationErrorCoordinator& errorCoordinator, WorkScheduler& workScheduler,
            MailboxMaintenanceRegistry& mailboxMaintenanceRegistry,
            javelin::app::undo::UndoManager& undoManager, QObject* parent = nullptr);

        void applySettings(std::vector<AccountSyncConfiguration> configurations);
        void setThreadMaterializationCoordinator(ThreadMaterializationCoordinator* coordinator);
        void
        setAuthenticationRefreshHandler(javelin::jmap::auth::AccessTokenRefreshHandler handler);
        void networkBecameReachable();
        [[nodiscard]] std::unordered_map<std::string, AccountSyncCoordinator::Status>
        accountStatuses() const;
        [[nodiscard]] std::optional<AccountConnectionSettings>
        connectionSettingsFor(std::string_view ownerAccountId) const override;
        [[nodiscard]] MailboxObservation observeMailbox(std::string accountId,
                                                        std::string mailboxId);
        [[nodiscard]] MailboxObservationLease
        beginMailboxObservation(std::string accountId, std::string mailboxId) override;
        [[nodiscard]] bool requestAccountSynchronization(std::string_view accountId);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        markMailNotificationsDelivered(std::string_view accountId, std::string_view mailboxId,
                                       const QStringList& emailIds);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        releaseMailNotificationDispatches(std::string_view accountId, const QStringList& emailIds);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        recoverMailNotificationDispatches();
        void publishMailboxWindowCommitted(QString accountId, QString mailboxId, std::size_t offset,
                                           std::size_t limit);
        void publishMessageContentCommitted(QString accountId, QString emailId);
        void publishThreadMaterializationCommitted(QString accountId, const QStringList& threadIds);
        [[nodiscard]] QCoro::Task<MailboxWindowResult>
        requestMailboxWindow(MailboxWindowIntent intent) override;
        [[nodiscard]] QCoro::Task<SearchWindowResult>
        requestSearchWindow(SearchWindowIntent intent) override;
        void ensureThread(ThreadMaterializationIntent intent) override;
        void retireSearchWindow(std::string accountId, std::string windowKey) override;
        [[nodiscard]] QCoro::Task<QueuedMailboxSelectionMutationResult>
        queueMailboxSelectionMutation(MailboxSelectionMutationIntent intent);
        [[nodiscard]] QCoro::Task<QueuedMessageSelectionMutationResult>
        queueDestroyMessages(std::string accountId, std::optional<std::string> sourceMailboxId,
                             MessageSelection selection);
        [[nodiscard]] QCoro::Task<QueuedMessageSelectionMutationResult>
        queueMarkMessagesUnread(std::string accountId, std::optional<std::string> sourceMailboxId,
                                MessageSelection selection);
        [[nodiscard]] QueuedMessageSelectionMutationResult queueMarkEmailRead(std::string accountId,
                                                                              std::string emailId);
        [[nodiscard]] QCoro::Task<QueuedMessageSelectionMutationResult>
        queueSetMessagesFlagged(std::string accountId, std::optional<std::string> sourceMailboxId,
                                MessageSelection selection, bool flagged);
        [[nodiscard]] QCoro::Task<QueuedMessageSelectionMutationResult>
        queueSetMessagesTag(std::string accountId, std::optional<std::string> sourceMailboxId,
                            MessageSelection selection, std::string keyword, bool enabled);
        [[nodiscard]] SaveMailTagDefinitionResult
        saveTagDefinition(SaveMailTagDefinition definition);
        [[nodiscard]] QueuedMailTagDeletionResult deleteTag(std::string accountId,
                                                            std::string keyword);
        [[nodiscard]] QCoro::Task<javelin::jmap::MailboxSubscriptionChangeResult>
        setMailboxSubscribed(std::string accountId, std::string mailboxId, bool subscribed);
        [[nodiscard]] QCoro::Task<javelin::jmap::MailboxCreateResult>
        createMailbox(std::string accountId, std::string name);
        [[nodiscard]] QCoro::Task<javelin::jmap::MailboxDestroyResult>
        destroyMailbox(std::string accountId, std::string mailboxId);
        [[nodiscard]] javelin::jmap::QueuedEmailMutationResult
        queueExactEmailMutation(std::string accountId,
                                javelin::jmap::EmailMailboxMutation mutation) override;
        [[nodiscard]] javelin::jmap::QueuedEmailMutationsResult queueExactEmailMutations(
            std::string accountId,
            std::vector<javelin::jmap::EmailMailboxMutation> mutations) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::SubmittedEmailMutationsResult>
        submitPendingEmailMutations(
            std::string accountId,
            std::optional<std::string> operationGroupId = std::nullopt) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::AuthoritativeEmailsResult>
        getAuthoritativeEmails(std::string accountId, std::vector<std::string> emailIds) override;
        [[nodiscard]] javelin::jmap::AuthoritativeEmailsResult
        getEffectiveEmails(std::string_view accountId,
                           std::span<const std::string> emailIds) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::MessageContentRefreshResult>
        requestMessageContent(std::string accountId, std::string emailId);
        [[nodiscard]] QCoro::Task<javelin::jmap::AttachmentDownloadResult>
        requestAttachment(std::string accountId, std::string emailId, std::string partId);
        [[nodiscard]] QCoro::Task<javelin::jmap::MessageSourceDownloadResult>
        requestMessageSource(std::string accountId, std::string emailId);
        [[nodiscard]] QCoro::Task<javelin::jmap::LiveRefreshResult>
        bootstrapAccount(AccountBootstrapIntent intent);
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactRefreshResult>
        requestContacts(std::string accountId) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarRefreshResult>
        requestCalendarRange(std::string ownerAccountId,
                             javelin::jmap::calendar::VisibleInterval interval,
                             javelin::jmap::calendar::TimeZoneId displayTimeZone);
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
        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveListResult>
        requestSieveScripts(std::string ownerAccountId) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveContentResult>
        requestSieveScript(std::string ownerAccountId,
                           javelin::jmap::sieve::SieveScript script) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveValidationResult>
        validateSieveScript(std::string ownerAccountId, QByteArray content);
        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveSaveResult>
        saveSieveScript(std::string ownerAccountId, javelin::jmap::sieve::SieveScript script,
                        QByteArray content,
                        javelin::app::undo::CommandOrigin origin =
                            javelin::app::undo::CommandOrigin::User) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveDeleteResult>
        deleteSieveScript(std::string ownerAccountId, javelin::jmap::sieve::SieveScript script,
                          javelin::app::undo::CommandOrigin origin =
                              javelin::app::undo::CommandOrigin::User) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveActivationResult>
        setSieveScriptActive(std::string ownerAccountId, javelin::jmap::sieve::SieveScript script,
                             bool active,
                             javelin::app::undo::CommandOrigin origin =
                                 javelin::app::undo::CommandOrigin::User) override;
        void publishCacheChange(javelin::app::MailCacheChange change) override;

      Q_SIGNALS:
        void accountStatusChanged(const QString& accountId,
                                  javelin::app::AccountSyncCoordinator::Status status);
        void sessionCapabilitiesChanged(const QString& ownerAccountId);
        void senderIdentityStateChanged(const QString& ownerAccountId);
        void cacheCommitted(javelin::app::MailCacheChange change);
        void threadMaterializationProgress(javelin::app::ThreadMaterializationProgress progress);
        void calendarCacheCommitted(javelin::app::CalendarCacheChange change);
        void notificationRaised(const QString& accountId, const QString& mailboxId,
                                const QString& threadId, const QString& emailId,
                                const QString& mailboxName, const QString& title,
                                const QString& message, const QStringList& deliveredEmailIds);

      private:
        friend class MailboxObservation;

        enum class SelectedMessageMutation
        {
            Destroy,
            MarkUnread,
        };

        [[nodiscard]] QCoro::Task<std::optional<javelin::jmap::OperationError>>
        ensureMessageSelectionMaterialized(std::string accountId,
                                           std::optional<std::string> sourceMailboxId,
                                           MessageSelection selection);
        [[nodiscard]] QueuedMailboxSelectionMutationResult
        queueResolvedMailboxSelectionMutation(MailboxSelectionMutationIntent intent);

        [[nodiscard]] QueuedMessageSelectionMutationResult
        queueSelectedMessageMutation(std::string accountId,
                                     std::optional<std::string> sourceMailboxId,
                                     MessageSelection selection, SelectedMessageMutation mutation);
        [[nodiscard]] QueuedMessageSelectionMutationResult
        queueSetMessagesKeyword(std::string accountId, std::optional<std::string> sourceMailboxId,
                                MessageSelection selection, std::string keyword, bool enabled,
                                QString historyVerb, bool appendKeywordToHistoryLabel);
        void connectCoordinator(const std::string& accountId, AccountSyncCoordinator& coordinator);
        void scheduleContactRefresh(std::string ownerAccountId);
        void restoreContactRefreshJobs();
        void scheduleContactRefreshPump();
        void pumpContactRefreshes();
        [[nodiscard]] QCoro::Task<void> runContactRefresh(std::string ownerAccountId,
                                                          std::string jobId);
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarRefreshResult>
        requestCalendarChanges(std::string ownerAccountId);
        void applyAccountConfiguration(const std::string& accountId);
        void refreshConfiguredSessions();
        void startSessionRefresh(const std::string& ownerAccountId,
                                 const AccountConnectionSettings& settings);
        void schedulePendingEmailMutationReplay(std::string accountId);
        void scheduleMailboxMutationReconciliation(std::string accountId);
        [[nodiscard]] QCoro::Task<void> reconcileMailboxMutations(std::string accountId);
        void scheduleTagDeletionPump();
        void pumpTagDeletions();
        [[nodiscard]] QCoro::Task<void> runTagDeletion(std::string jobId, std::string accountId,
                                                       std::string keyword);
        void releaseMailboxObservation(
            javelin::jmap::sync::MailboxInterestRegistry::ObservationId observationId);
        [[nodiscard]] bool beginSearchWindowRequest(const std::string& leaseKey);
        void finishSearchWindowRequest(const std::string& leaseKey);
        [[nodiscard]] bool searchWindowRetired(const std::string& leaseKey) const;

        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::JmapCore& m_jmapCore;
        javelin::jmap::api::JmapMethodTransport& m_methodTransport;
        QNetworkAccessManager& m_networkAccessManager;
        javelin::jmap::api::WebSocketFailureCooldowns& m_transportCooldowns;
        javelin::jmap::cache::AccountRepository& m_accountRepository;
        javelin::jmap::cache::MailboxReader& m_mailboxReader;
        javelin::jmap::cache::MailTagReader& m_mailTagReader;
        javelin::jmap::cache::MailboxStatisticsReader& m_mailboxStatisticsReader;
        javelin::jmap::cache::MailboxMessageReader& m_mailboxMessageReader;
        javelin::jmap::cache::MailboxFilterReader& m_mailboxFilterReader;
        javelin::jmap::cache::ContactReader& m_contactReader;
        javelin::jmap::contacts::ContactService& m_contactService;
        javelin::jmap::calendar::CalendarService& m_calendarService;
        javelin::jmap::sieve::SieveService& m_sieveService;
        ApplicationErrorCoordinator& m_errorCoordinator;
        WorkScheduler& m_workScheduler;
        MailboxMaintenanceRegistry& m_mailboxMaintenanceRegistry;
        ThreadMaterializationCoordinator* m_threadMaterializationCoordinator = nullptr;
        javelin::jmap::auth::AccessTokenRefreshHandler m_authenticationRefreshHandler;
        javelin::app::undo::UndoManager& m_undoManager;
        struct VisibleCalendarRange
        {
            javelin::jmap::calendar::VisibleInterval interval;
            javelin::jmap::calendar::TimeZoneId displayTimeZone;
        };
        std::unordered_map<std::string, VisibleCalendarRange> m_visibleCalendarRanges;
        std::unordered_map<std::string, std::unique_ptr<AccountSyncCoordinator>> m_coordinators;
        std::unordered_map<std::string, AccountSyncConfiguration> m_configurations;
        std::unordered_set<std::string> m_sessionRefreshesInFlight;
        std::unordered_set<std::string> m_pendingMutationReplaysInFlight;
        std::unordered_set<std::string> m_mailboxMutationReconciliationsInFlight;
        std::unordered_set<std::string> m_pendingContactRefreshes;
        std::unordered_set<std::string> m_runningContactRefreshes;
        std::unordered_set<std::string> m_runningTagDeletions;
        struct SearchWindowRequestState
        {
            std::size_t activeRequests = 0;
            bool retired = false;
        };
        std::unordered_map<std::string, SearchWindowRequestState> m_searchWindowRequests;
        bool m_contactRefreshPumpScheduled = false;
        bool m_tagDeletionPumpScheduled = false;
        javelin::jmap::sync::MailboxInterestRegistry m_mailboxInterests;
    };

    class MailboxObservation final
    {
      public:
        MailboxObservation() = default;
        ~MailboxObservation();

        MailboxObservation(const MailboxObservation&) = delete;
        MailboxObservation& operator=(const MailboxObservation&) = delete;
        MailboxObservation(MailboxObservation&& other) noexcept;
        MailboxObservation& operator=(MailboxObservation&& other) noexcept;

        void reset();
        [[nodiscard]] explicit operator bool() const;

      private:
        friend class MailApplicationService;

        MailboxObservation(
            MailApplicationService& service,
            javelin::jmap::sync::MailboxInterestRegistry::ObservationId observationId);

        QPointer<MailApplicationService> m_service;
        javelin::jmap::sync::MailboxInterestRegistry::ObservationId m_observationId = 0;
    };

} // namespace javelin::app
