#pragma once

#include "app/AccountConnectionSettings.h"
#include "app/LongPollService.h"
#include "app/MailboxSelectionMutation.h"
#include "jmap/calendar/CalendarService.h"
#include "jmap/contacts/ContactService.h"
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

namespace javelin::app
{
    class ApplicationErrorCoordinator;

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
    };

    using QueuedMailboxSelectionMutationResult =
        std::variant<QueuedMailboxSelectionMutation, javelin::jmap::OperationError>;

    class MailApplicationService final : public QObject
    {
        Q_OBJECT

      public:
        MailApplicationService(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                               javelin::jmap::JmapCore& jmapCore,
                               javelin::jmap::api::JmapMethodTransport& methodTransport,
                               QNetworkAccessManager& networkAccessManager,
                               javelin::jmap::cache::AccountRepository& accountRepository,
                               javelin::jmap::cache::QueryService& queryService,
                               javelin::jmap::contacts::ContactService& contactService,
                               javelin::jmap::calendar::CalendarService& calendarService,
                               javelin::jmap::sieve::SieveService& sieveService,
                               ApplicationErrorCoordinator& errorCoordinator,
                               QObject* parent = nullptr);

        void applySettings(std::vector<AccountSyncConfiguration> configurations);
        [[nodiscard]] MailboxObservation observeMailbox(std::string accountId,
                                                        std::string mailboxId);
        [[nodiscard]] bool requestAccountSynchronization(std::string_view accountId);
        [[nodiscard]] QString statusSummary() const;
        [[nodiscard]] QCoro::Task<MailboxWindowResult>
        requestMailboxWindow(MailboxWindowIntent intent);
        [[nodiscard]] QCoro::Task<SearchWindowResult>
        requestSearchWindow(SearchWindowIntent intent);
        [[nodiscard]] javelin::jmap::QueuedEmailMutationResult
        queueDestroyEmail(std::string accountId, std::string emailId);
        [[nodiscard]] javelin::jmap::QueuedEmailMutationResult
        queueMoveEmail(std::string accountId, std::string emailId, std::string sourceMailboxId,
                       std::string destinationMailboxId);
        [[nodiscard]] javelin::jmap::QueuedEmailMutationResult
        queueCopyEmail(std::string accountId, std::string emailId, std::string sourceMailboxId,
                       std::string destinationMailboxId);
        [[nodiscard]] QueuedMailboxSelectionMutationResult
        queueMailboxSelectionMutation(MailboxSelectionMutationIntent intent);
        [[nodiscard]] javelin::jmap::QueuedEmailMutationResult
        queueMarkEmailRead(std::string accountId, std::string emailId);
        [[nodiscard]] javelin::jmap::QueuedEmailMutationResult
        queueMarkEmailUnread(std::string accountId, std::string emailId);
        [[nodiscard]] javelin::jmap::QueuedEmailMutationResult
        queueSetEmailFlagged(std::string accountId, std::string emailId, bool flagged);
        [[nodiscard]] QCoro::Task<javelin::jmap::SubmittedEmailMutationsResult>
        submitPendingEmailMutations(std::string accountId);
        [[nodiscard]] QCoro::Task<javelin::jmap::MessageContentRefreshResult>
        requestMessageContent(std::string accountId, std::string emailId);
        [[nodiscard]] QCoro::Task<javelin::jmap::AttachmentDownloadResult>
        requestAttachment(std::string accountId, std::string emailId, std::string partId);
        [[nodiscard]] QCoro::Task<javelin::jmap::MessageSourceDownloadResult>
        requestMessageSource(std::string accountId, std::string emailId);
        [[nodiscard]] QCoro::Task<javelin::jmap::LiveRefreshResult>
        bootstrapAccount(AccountBootstrapIntent intent);
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactRefreshResult>
        requestContacts(std::string accountId);
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarRefreshResult>
        requestCalendarRange(std::string ownerAccountId,
                             javelin::jmap::calendar::VisibleInterval interval,
                             javelin::jmap::calendar::TimeZoneId displayTimeZone);
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        createCalendarEvent(std::string ownerAccountId,
                            javelin::jmap::calendar::CreateEventCommand command);
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        updateCalendarEvent(std::string ownerAccountId,
                            javelin::jmap::calendar::UpdateEventCommand command);
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        deleteCalendarEvent(std::string ownerAccountId,
                            javelin::jmap::calendar::DeleteEventCommand command);
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        setDefaultCalendar(std::string ownerAccountId, std::string accountId,
                           std::string calendarId);
        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveListResult>
        requestSieveScripts(std::string ownerAccountId);
        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveContentResult>
        requestSieveScript(std::string ownerAccountId, javelin::jmap::sieve::SieveScript script);
        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveValidationResult>
        validateSieveScript(std::string ownerAccountId, QByteArray content);
        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveSaveResult>
        saveSieveScript(std::string ownerAccountId, javelin::jmap::sieve::SieveScript script,
                        QByteArray content);
        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveDeleteResult>
        deleteSieveScript(std::string ownerAccountId, javelin::jmap::sieve::SieveScript script);
        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveActivationResult>
        setSieveScriptActive(std::string ownerAccountId, javelin::jmap::sieve::SieveScript script,
                             bool active);
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        setAddressBooks(std::string accountId, javelin::jmap::api::AddressBookSetRequest request);
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        setContactCards(std::string accountId, javelin::jmap::api::ContactCardSetRequest request);
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        createContactGroup(std::string ownerAccountId,
                           javelin::jmap::contacts::CreateContactGroupCommand command);
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        setContactGroupMembership(
            std::string ownerAccountId,
            javelin::jmap::contacts::SetContactGroupMembershipCommand command);
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        copyContactCards(std::string accountId, javelin::jmap::api::ContactCardCopyRequest request);
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactUploadResult>
        uploadContactMedia(std::string ownerAccountId, std::string accountId, QByteArray payload,
                           std::string mediaType);
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactDownloadResult>
        downloadContactMedia(std::string ownerAccountId, std::string accountId, std::string blobId,
                             std::string mediaType);
        void stop();

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

        void connectCoordinator(const std::string& accountId, AccountSyncCoordinator& coordinator);
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
        javelin::jmap::cache::AccountRepository& m_accountRepository;
        javelin::jmap::cache::QueryService& m_queryService;
        javelin::jmap::contacts::ContactService& m_contactService;
        javelin::jmap::calendar::CalendarService& m_calendarService;
        javelin::jmap::sieve::SieveService& m_sieveService;
        ApplicationErrorCoordinator& m_errorCoordinator;
        struct VisibleCalendarRange
        {
            javelin::jmap::calendar::VisibleInterval interval;
            javelin::jmap::calendar::TimeZoneId displayTimeZone;
        };
        std::unordered_map<std::string, VisibleCalendarRange> m_visibleCalendarRanges;
        std::unordered_map<std::string, std::unique_ptr<AccountSyncCoordinator>> m_coordinators;
        std::unordered_map<std::string, AccountSyncConfiguration> m_configurations;
        std::unordered_set<std::string> m_sessionRefreshesInFlight;
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
