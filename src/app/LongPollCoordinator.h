#pragma once

#include "app/AccountConnectionSettings.h"
#include "app/LongPollService.h"
#include "jmap/calendar/CalendarService.h"
#include "jmap/contacts/ContactService.h"
#include "jmap/query/EmailListSort.h"
#include "jmap/search/EmailSearch.h"
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

    class MailboxObservation;

    struct MailboxWindowIntent
    {
        std::string accountId;
        std::string mailboxId;
        std::size_t offset = 0;
        std::size_t limit = 0;
        javelin::jmap::query::EmailListSort sort;
        bool forceRefresh = false;
    };

    struct SearchWindowIntent
    {
        std::string accountId;
        javelin::jmap::search::EmailSearchCriteria criteria;
        std::size_t offset = 0;
        std::size_t limit = 0;
        javelin::jmap::query::EmailListSort sort;
    };

    struct MailboxWindowSummary
    {
        std::string accountId;
        std::string mailboxId;
        std::size_t offset = 0;
        std::size_t limit = 0;
        std::size_t representativeCount = 0;
        std::optional<std::size_t> total;
    };

    using MailboxWindowResult = std::variant<MailboxWindowSummary, javelin::jmap::LiveRefreshError>;

    struct SearchWindowSummary
    {
        std::string accountId;
        std::string queryKey;
        std::size_t offset = 0;
        std::size_t limit = 0;
        std::size_t representativeCount = 0;
        std::optional<std::size_t> total;
    };

    using SearchWindowResult = std::variant<SearchWindowSummary, javelin::jmap::LiveRefreshError>;

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
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        setAddressBooks(std::string accountId, javelin::jmap::api::AddressBookSetRequest request);
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        setContactCards(std::string accountId, javelin::jmap::api::ContactCardSetRequest request);
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        copyContactCards(std::string accountId, javelin::jmap::api::ContactCardCopyRequest request);
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactUploadResult>
        uploadContactMedia(std::string ownerAccountId, std::string accountId, QByteArray payload,
                           std::string mediaType);
        void stop();

      Q_SIGNALS:
        void accountStatusChanged(const QString& accountId,
                                  javelin::app::AccountSyncCoordinator::Status status);
        void cacheCommitted(javelin::app::MailCacheChange change);
        void calendarCacheCommitted(javelin::app::CalendarCacheChange change);
        void notificationRaised(const QString& accountId, const QString& mailboxId,
                                const QString& threadId, const QString& emailId,
                                const QString& mailboxName, const QString& title,
                                const QString& message);

      private:
        friend class MailboxObservation;

        void connectCoordinator(const std::string& accountId, AccountSyncCoordinator& coordinator);
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
