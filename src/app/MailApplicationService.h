#pragma once

#include "app/AccountConnectionProvider.h"
#include "app/AccountConnectionSettings.h"
#include "app/ContactApplicationPorts.h"
#include "app/LongPollService.h"
#include "app/MailboxSelectionMutation.h"
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

namespace javelin::app::undo
{
    class UndoManager;
}

namespace javelin::app
{
    class ApplicationErrorCoordinator;
    class WorkScheduler;

    class MailboxObservation;

    struct MailboxWindowIntent
    {
        std::string accountId;
        std::string mailboxId;
        std::size_t offset = 0;
        std::size_t limit = 0;
        javelin::jmap::query::EmailListSort sort;
        bool forceRefresh = false;
        std::optional<std::string> anchor;
        std::int64_t anchorOffset = 1;
    };

    struct SearchWindowIntent
    {
        std::string accountId;
        javelin::jmap::search::EmailSearchCriteria criteria;
        std::size_t offset = 0;
        std::size_t limit = 0;
        javelin::jmap::query::EmailListSort sort;
        std::optional<std::string> anchor;
        std::string windowKey;
    };

    struct MailboxWindowSummary
    {
        std::string accountId;
        std::string mailboxId;
        std::size_t offset = 0;
        std::size_t limit = 0;
        std::size_t position = 0;
        std::size_t returnedLimit = 0;
        std::size_t representativeCount = 0;
        std::optional<std::size_t> total;
        std::string queryState;
    };

    using MailboxWindowResult = std::variant<MailboxWindowSummary, javelin::jmap::OperationError>;

    struct SearchWindowSummary
    {
        std::string accountId;
        std::string queryKey;
        std::size_t offset = 0;
        std::size_t limit = 0;
        std::size_t position = 0;
        std::size_t returnedLimit = 0;
        std::size_t representativeCount = 0;
        std::optional<std::size_t> total;
        std::string queryState;
    };

    using SearchWindowResult = std::variant<SearchWindowSummary, javelin::jmap::OperationError>;

    struct CalendarCacheChange
    {
        QString ownerAccountId;
        javelin::jmap::calendar::VisibleInterval interval;
        javelin::jmap::calendar::TimeZoneId displayTimeZone;
        std::size_t accountCount = 0;
        std::size_t eventCount = 0;
    };

    struct AccountSyncConfiguration
    {
        AccountConnectionSettings settings;
        std::string accountId;
        std::vector<std::string> mailboxIds;
        std::vector<std::string> fullSyncMailboxIds;
        std::vector<std::string> notificationMailboxIds;
        bool notificationMailboxSelectionConfigured = false;
    };

    struct AccountBootstrapIntent
    {
        AccountConnectionSettings settings;
        std::vector<std::string> mailboxIds;
    };

    struct QueuedMailboxSelectionMutation
    {
        std::string accountId;
        std::size_t queuedEmailCount = 0;
        std::size_t skippedEmailCount = 0;
        std::vector<javelin::jmap::QueuedEmailMutation> queuedMutations;
        std::optional<QString> historyEntryId;
    };

    using QueuedMailboxSelectionMutationResult =
        std::variant<QueuedMailboxSelectionMutation, javelin::jmap::OperationError>;

    struct QueuedMessageSelectionMutation
    {
        std::string accountId;
        std::size_t queuedEmailCount = 0;
        std::vector<javelin::jmap::QueuedEmailMutation> queuedMutations;
        std::optional<QString> historyEntryId;
    };

    using QueuedMessageSelectionMutationResult =
        std::variant<QueuedMessageSelectionMutation, javelin::jmap::OperationError>;

    class MailApplicationService final : public QObject,
                                         public AccountConnectionProvider,
                                         public ContactRefreshPort,
                                         public javelin::app::undo::MailHistoryPort,
                                         public javelin::app::undo::SieveHistoryPort,
                                         public javelin::app::undo::CalendarHistoryPort,
                                         public javelin::app::undo::CalendarPreferencePort
    {
        Q_OBJECT

      public:
        MailApplicationService(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                               javelin::jmap::JmapCore& jmapCore,
                               javelin::jmap::api::JmapMethodTransport& methodTransport,
                               QNetworkAccessManager& networkAccessManager,
                               javelin::jmap::api::WebSocketFailureCooldowns& cooldowns,
                               javelin::jmap::cache::AccountRepository& accountRepository,
                               javelin::jmap::cache::QueryService& queryService,
                               javelin::jmap::contacts::ContactService& contactService,
                               javelin::jmap::calendar::CalendarService& calendarService,
                               javelin::jmap::sieve::SieveService& sieveService,
                               ApplicationErrorCoordinator& errorCoordinator,
                               WorkScheduler& workScheduler,
                               javelin::app::undo::UndoManager& undoManager,
                               QObject* parent = nullptr);

        void applySettings(std::vector<AccountSyncConfiguration> configurations);
        [[nodiscard]] std::optional<AccountConnectionSettings>
        connectionSettingsFor(std::string_view ownerAccountId) const override;
        [[nodiscard]] MailboxObservation observeMailbox(std::string accountId,
                                                        std::string mailboxId);
        [[nodiscard]] bool requestAccountSynchronization(std::string_view accountId);
        void publishMailboxWindowCommitted(QString accountId, QString mailboxId, std::size_t offset,
                                           std::size_t limit);
        [[nodiscard]] QCoro::Task<MailboxWindowResult>
        requestMailboxWindow(MailboxWindowIntent intent);
        [[nodiscard]] QCoro::Task<SearchWindowResult>
        requestSearchWindow(SearchWindowIntent intent);
        void retireSearchWindow(std::string accountId, std::string windowKey);
        [[nodiscard]] QueuedMailboxSelectionMutationResult
        queueMailboxSelectionMutation(MailboxSelectionMutationIntent intent);
        [[nodiscard]] QueuedMessageSelectionMutationResult
        queueDestroyMessages(std::string accountId, std::optional<std::string> sourceMailboxId,
                             MessageSelection selection);
        [[nodiscard]] QueuedMessageSelectionMutationResult
        queueMarkMessagesUnread(std::string accountId, std::optional<std::string> sourceMailboxId,
                                MessageSelection selection);
        [[nodiscard]] QueuedMessageSelectionMutationResult queueMarkEmailRead(std::string accountId,
                                                                              std::string emailId);
        [[nodiscard]] QueuedMessageSelectionMutationResult
        queueSetEmailFlagged(std::string accountId, std::string emailId, bool flagged);
        [[nodiscard]] javelin::jmap::QueuedEmailMutationResult
        queueExactEmailMutation(std::string accountId,
                                javelin::jmap::EmailMailboxMutation mutation) override;
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
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::AuthoritativeCalendarEventResult>
        getAuthoritativeCalendarEvent(std::string ownerAccountId, std::string accountId,
                                      std::optional<std::string> eventId, std::string uid) override;
        [[nodiscard]] javelin::jmap::calendar::AuthoritativeCalendarEventResult
        getEffectiveCalendarEvent(std::string_view accountId,
                                  const std::optional<std::string>& eventId) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        setDefaultCalendar(
            std::string ownerAccountId, std::string accountId, std::string calendarId,
            javelin::app::undo::CommandOrigin origin = javelin::app::undo::CommandOrigin::User);
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
      Q_SIGNALS:
        void accountStatusChanged(const QString& accountId,
                                  javelin::app::AccountSyncCoordinator::Status status);
        void sessionCapabilitiesChanged(const QString& ownerAccountId);
        void cacheCommitted(javelin::app::MailCacheChange change);
        void calendarCacheCommitted(javelin::app::CalendarCacheChange change);
        void notificationRaised(const QString& accountId, const QString& mailboxId,
                                const QString& threadId, const QString& emailId,
                                const QString& mailboxName, const QString& title,
                                const QString& message);

      private:
        friend class MailboxObservation;

        enum class SelectedMessageMutation
        {
            Destroy,
            MarkUnread,
        };

        [[nodiscard]] QueuedMessageSelectionMutationResult
        queueSelectedMessageMutation(std::string accountId,
                                     std::optional<std::string> sourceMailboxId,
                                     MessageSelection selection, SelectedMessageMutation mutation);
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
        void releaseMailboxObservation(
            javelin::jmap::sync::MailboxInterestRegistry::ObservationId observationId);

        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::JmapCore& m_jmapCore;
        javelin::jmap::api::JmapMethodTransport& m_methodTransport;
        QNetworkAccessManager& m_networkAccessManager;
        javelin::jmap::api::WebSocketFailureCooldowns& m_transportCooldowns;
        javelin::jmap::cache::AccountRepository& m_accountRepository;
        javelin::jmap::cache::QueryService& m_queryService;
        javelin::jmap::contacts::ContactService& m_contactService;
        javelin::jmap::calendar::CalendarService& m_calendarService;
        javelin::jmap::sieve::SieveService& m_sieveService;
        ApplicationErrorCoordinator& m_errorCoordinator;
        WorkScheduler& m_workScheduler;
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
        std::unordered_set<std::string> m_pendingContactRefreshes;
        std::unordered_set<std::string> m_runningContactRefreshes;
        std::unordered_set<std::string> m_retiredSearchWindowKeys;
        bool m_contactRefreshPumpScheduled = false;
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
