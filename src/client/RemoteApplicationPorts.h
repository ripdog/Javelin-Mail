#pragma once

#include "storage/DatabaseError.h"

#include "app/AccountApplicationPorts.h"
#include "app/AccountRefreshApplicationPorts.h"
#include "app/CalendarApplicationPorts.h"
#include "app/ComposeApplicationPorts.h"
#include "app/ContactApplicationPorts.h"
#include "app/DeveloperDiagnostics.h"
#include "app/DeveloperMaintenance.h"
#include "app/IdentityApplicationPorts.h"
#include "app/LogStore.h"
#include "app/MailApplicationPorts.h"
#include "app/MessageContentApplicationPorts.h"
#include "app/MessageListMaterializationPort.h"
#include "app/SieveApplicationPorts.h"
#include "app/UndoApplicationPorts.h"
#include "app/WorkTaskPort.h"
#include "client/RemoteActionClient.h"
#include "jmap/calendar/CalendarReader.h"

#include <QObject>

namespace javelin::app
{
    class GuiDaemonSession;

    class RemoteAccountCommandPort final : public AccountCommandPort
    {
      public:
        explicit RemoteAccountCommandPort(RemoteActionClient& client);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        removeConfiguredAccount(const QString& loginEmail, const QString& sessionUrl,
                                const QStringList& knownAccountIds) override;

      private:
        RemoteActionClient& m_client;
    };

    class RemoteCalendarReader final : public javelin::jmap::calendar::CalendarReader
    {
      public:
        explicit RemoteCalendarReader(RemoteActionClient& client);
        [[nodiscard]] javelin::jmap::calendar::CalendarLoadResult
        loadCached(std::string_view accountId,
                   const javelin::jmap::calendar::VisibleInterval& interval,
                   const javelin::jmap::calendar::TimeZoneId& displayTimeZone) const override;
        [[nodiscard]] javelin::jmap::calendar::CalendarAccountsResult accounts() const override;
        [[nodiscard]] javelin::jmap::calendar::CalendarListResult
        calendars(std::string_view accountId) const override;

      private:
        RemoteActionClient& m_client;
    };

    class RemoteCalendarCommandPort final : public CalendarCommandPort
    {
        Q_OBJECT

      public:
        RemoteCalendarCommandPort(RemoteActionClient& client, QObject* parent = nullptr);
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarRefreshResult>
        requestCalendarRange(std::string ownerAccountId,
                             javelin::jmap::calendar::VisibleInterval interval,
                             javelin::jmap::calendar::TimeZoneId displayTimeZone) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        createCalendarEvent(std::string ownerAccountId,
                            javelin::jmap::calendar::CreateEventCommand command,
                            undo::CommandOrigin origin) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        updateCalendarEvent(std::string ownerAccountId,
                            javelin::jmap::calendar::UpdateEventCommand command,
                            undo::CommandOrigin origin) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        deleteCalendarEvent(std::string ownerAccountId,
                            javelin::jmap::calendar::DeleteEventCommand command,
                            undo::CommandOrigin origin) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        respondToCalendarEvent(std::string ownerAccountId,
                               javelin::jmap::calendar::RespondToEventCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        setCalendarSubscribed(std::string ownerAccountId, std::string accountId,
                              std::string calendarId, bool subscribed) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        setDefaultCalendar(std::string ownerAccountId, std::string accountId,
                           std::string calendarId, undo::CommandOrigin origin) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        createCalendar(std::string ownerAccountId,
                       javelin::jmap::calendar::CreateCalendarCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        deleteCalendar(std::string ownerAccountId,
                       javelin::jmap::calendar::DeleteCalendarCommand command) override;
        [[nodiscard]] javelin::jmap::calendar::CalendarPreferenceResult
        setCalendarVisible(std::string accountId, std::string calendarId, bool visible,
                           undo::CommandOrigin origin) override;

      private:
        void noteCalendarChanged(const std::string& ownerAccountId);
        RemoteActionClient& m_client;
    };

    class RemoteComposeCommandPort final : public ComposeCommandPort
    {
      public:
        explicit RemoteComposeCommandPort(RemoteActionClient& client);
        [[nodiscard]] QCoro::Task<
            std::variant<javelin::jmap::submission::DraftSnapshot, javelin::jmap::OperationError>>
        open(AccountConnectionSettings settings,
             javelin::jmap::submission::OpenComposeRequest request) override;
        [[nodiscard]] QCoro::Task<std::variant<std::vector<javelin::jmap::domain::Identity>,
                                               javelin::jmap::OperationError>>
        loadSenderIdentities(AccountConnectionSettings settings, std::string accountId) override;
        [[nodiscard]] QCoro::Task<std::variant<javelin::jmap::submission::DraftSaveSummary,
                                               javelin::jmap::OperationError>>
        saveDraft(AccountConnectionSettings settings,
                  javelin::jmap::submission::DraftSnapshot snapshot) override;
        [[nodiscard]] QCoro::Task<
            std::variant<javelin::jmap::submission::SendSummary, javelin::jmap::OperationError>>
        send(AccountConnectionSettings settings,
             javelin::jmap::submission::DraftSnapshot snapshot) override;
        [[nodiscard]] QCoro::Task<
            std::variant<javelin::jmap::submission::SendSummary, javelin::jmap::OperationError>>
        scheduleSend(AccountConnectionSettings settings,
                     javelin::jmap::submission::ScheduledSendRequest request) override;
        [[nodiscard]] std::variant<std::optional<javelin::jmap::submission::DraftSnapshot>,
                                   javelin::jmap::OperationError>
        loadWorkingCopy(std::string_view composeSessionId) const override;
        [[nodiscard]] std::optional<javelin::jmap::OperationError>
        storeWorkingCopy(const javelin::jmap::submission::DraftSnapshot& snapshot) override;
        [[nodiscard]] std::optional<javelin::jmap::OperationError>
        discard(std::string_view composeSessionId) override;

      private:
        RemoteActionClient& m_client;
    };

    class RemoteContactCommandPort final : public ContactCommandPort
    {
      public:
        explicit RemoteContactCommandPort(RemoteActionClient& client);
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        mutateAddressBook(std::string ownerAccountId, AddressBookCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        saveContact(std::string ownerAccountId, SaveContactCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        setContactsStarred(std::string ownerAccountId, SetContactsStarredCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        deleteContacts(std::string ownerAccountId, DeleteContactsCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        createContactGroup(std::string ownerAccountId, CreateContactGroupCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        deleteContactGroup(std::string ownerAccountId, DeleteContactGroupCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        setContactGroupMembership(std::string ownerAccountId,
                                  SetContactGroupMembershipCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        copyContact(std::string ownerAccountId, CopyContactCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        importContacts(std::string ownerAccountId, ImportContactsCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        mergeContacts(std::string ownerAccountId, MergeContactsCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactUploadResult>
        uploadContactMedia(std::string ownerAccountId, std::string accountId, QByteArray payload,
                           std::string mediaType) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactDownloadResult>
        downloadContactMedia(std::string ownerAccountId, std::string accountId, std::string blobId,
                             std::string mediaType) override;

      private:
        RemoteActionClient& m_client;
    };

    class RemoteMailCommandPort final : public MailCommandPort
    {
      public:
        explicit RemoteMailCommandPort(RemoteActionClient& client);
        [[nodiscard]] QCoro::Task<QueuedMailboxSelectionMutationResult>
        queueMailboxSelectionMutation(MailboxSelectionMutationIntent intent) override;
        [[nodiscard]] QCoro::Task<QueuedMessageSelectionMutationResult>
        queueDestroyMessages(std::string accountId, std::optional<std::string> sourceMailboxId,
                             MessageSelection selection) override;
        [[nodiscard]] QCoro::Task<QueuedMessageSelectionMutationResult>
        queueMarkMessagesUnread(std::string accountId, std::optional<std::string> sourceMailboxId,
                                MessageSelection selection) override;
        [[nodiscard]] QCoro::Task<QueuedMessageSelectionMutationResult>
        queueMarkEmailRead(std::string accountId, std::string emailId) override;
        [[nodiscard]] QCoro::Task<QueuedMessageSelectionMutationResult>
        queueSetMessagesFlagged(std::string accountId, std::optional<std::string> sourceMailboxId,
                                MessageSelection selection, bool flagged) override;
        [[nodiscard]] QCoro::Task<QueuedMessageSelectionMutationResult>
        queueSetMessagesTag(std::string accountId, std::optional<std::string> sourceMailboxId,
                            MessageSelection selection, std::string keyword, bool enabled) override;
        [[nodiscard]] QCoro::Task<SaveMailTagDefinitionResult>
        saveTagDefinition(SaveMailTagDefinition definition) override;
        [[nodiscard]] QCoro::Task<QueuedMailTagDeletionResult>
        deleteTag(std::string accountId, std::string keyword) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::MailboxSubscriptionChangeResult>
        setMailboxSubscribed(std::string accountId, std::string mailboxId,
                             bool subscribed) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::MailboxCreateResult>
        createMailbox(std::string accountId, std::string name) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::MailboxDestroyResult>
        destroyMailbox(std::string accountId, std::string mailboxId) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::SubmittedEmailMutationsResult>
        submitPendingEmailMutations(std::string accountId,
                                    std::optional<std::string> operationGroupId) override;

      private:
        RemoteActionClient& m_client;
    };

    class RemoteSieveCommandPort final : public SieveCommandPort
    {
      public:
        explicit RemoteSieveCommandPort(RemoteActionClient& client);
        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveListResult>
        requestSieveScripts(std::string ownerAccountId) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveContentResult>
        requestSieveScript(std::string ownerAccountId,
                           javelin::jmap::sieve::SieveScript script) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveValidationResult>
        validateSieveScript(std::string ownerAccountId, QByteArray content) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveSaveResult>
        saveSieveScript(std::string ownerAccountId, javelin::jmap::sieve::SieveScript script,
                        QByteArray content, undo::CommandOrigin origin) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveDeleteResult>
        deleteSieveScript(std::string ownerAccountId, javelin::jmap::sieve::SieveScript script,
                          undo::CommandOrigin origin) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveActivationResult>
        setSieveScriptActive(std::string ownerAccountId, javelin::jmap::sieve::SieveScript script,
                             bool active, undo::CommandOrigin origin) override;

      private:
        RemoteActionClient& m_client;
    };

    class RemoteIdentityCommandPort final : public IdentityCommandPort
    {
      public:
        explicit RemoteIdentityCommandPort(RemoteActionClient& client);
        [[nodiscard]] QCoro::Task<javelin::jmap::identity::IdentityListResult>
        requestSenderIdentities(std::string accountId) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::identity::IdentitySaveResult>
        saveSenderIdentity(std::string accountId,
                           javelin::jmap::domain::Identity identity) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::identity::IdentityDeleteResult>
        deleteSenderIdentity(std::string accountId, std::string identityId) override;

      private:
        RemoteActionClient& m_client;
    };

    class RemoteAccountRefreshPort final : public AccountRefreshPort
    {
      public:
        RemoteAccountRefreshPort(GuiDaemonSession& session, RemoteActionClient& client);
        [[nodiscard]] bool requestAccountSynchronization(std::string_view accountId) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::LiveRefreshResult>
        bootstrapAccount(AccountBootstrapIntent intent) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactRefreshResult>
        requestContacts(std::string ownerAccountId) override;

      private:
        GuiDaemonSession& m_session;
        RemoteActionClient& m_client;
    };

    class RemoteMessageListMaterializationPort final : public MessageListMaterializationPort
    {
      public:
        explicit RemoteMessageListMaterializationPort(RemoteActionClient& client);
        [[nodiscard]] MailboxObservationLease
        beginMailboxObservation(std::string accountId, std::string mailboxId) override;
        [[nodiscard]] QCoro::Task<MailboxWindowResult>
        requestMailboxWindow(MailboxWindowIntent intent) override;
        [[nodiscard]] QCoro::Task<SearchWindowResult>
        requestSearchWindow(SearchWindowIntent intent) override;
        void ensureThread(ThreadMaterializationIntent intent) override;
        void retireSearchWindow(std::string accountId, std::string windowKey) override;

      private:
        RemoteActionClient& m_client;
    };

    class RemoteMessageContentPort final : public MessageContentPort
    {
      public:
        explicit RemoteMessageContentPort(RemoteActionClient& client);
        [[nodiscard]] QCoro::Task<javelin::jmap::MessageContentRefreshResult>
        requestMessageContent(std::string accountId, std::string emailId) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::AttachmentDownloadResult>
        requestAttachment(std::string accountId, std::string emailId, std::string partId) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::MessageSourceDownloadResult>
        requestMessageSource(std::string accountId, std::string emailId) override;

      private:
        RemoteActionClient& m_client;
    };

    class RemoteUndoCommandPort final : public UndoCommandPort
    {
        Q_OBJECT

      public:
        RemoteUndoCommandPort(GuiDaemonSession& session, RemoteActionClient& client,
                              QObject* parent = nullptr);
        [[nodiscard]] QCoro::Task<bool> undo() override;
        [[nodiscard]] QCoro::Task<bool> redo() override;
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        acknowledgeAndRemove(const QString& entryId) override;
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        forget(const QString& entryId) override;
        [[nodiscard]] const undo::HistoryState& state() const override;
        [[nodiscard]] const std::vector<undo::HistoryEntry>& entries() const override;

      private:
        void refreshSnapshot();
        GuiDaemonSession& m_session;
        RemoteActionClient& m_client;
        undo::HistoryState m_state;
        std::vector<undo::HistoryEntry> m_entries;
    };

    class RemoteDeveloperDiagnosticsPort final : public DeveloperDiagnosticsPort
    {
      public:
        explicit RemoteDeveloperDiagnosticsPort(RemoteActionClient& client);
        [[nodiscard]] QCoro::Task<DeveloperDiagnosticsResult> snapshot() override;

      private:
        RemoteActionClient& m_client;
    };

    class RemoteDeveloperMaintenancePort final : public DeveloperMaintenancePort
    {
      public:
        explicit RemoteDeveloperMaintenancePort(RemoteActionClient& client);
        [[nodiscard]] QCoro::Task<DeveloperMailboxClearResult>
        clearMailboxCache(DeveloperMailboxClearCommand command) override;

      private:
        RemoteActionClient& m_client;
    };

    class RemoteDaemonLogPort final : public DaemonLogPort
    {
      public:
        RemoteDaemonLogPort(GuiDaemonSession& session, RemoteActionClient& client,
                            QObject* parent = nullptr);
        [[nodiscard]] QVector<LogEntry> entries() const override;
        void acquire() override;
        void release() override;
        void clear() override;

      private:
        void setRemoteSubscribed(bool subscribed);

        GuiDaemonSession& m_session;
        RemoteActionClient& m_client;
        QVector<LogEntry> m_entries;
        std::size_t m_subscribers = 0;
    };

    class RemoteWorkTaskPort final : public WorkTaskPort
    {
      public:
        RemoteWorkTaskPort(GuiDaemonSession& session, RemoteActionClient& client);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        pause(std::string_view jobId) override;
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        resume(std::string_view jobId) override;
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        retry(std::string_view jobId) override;
        [[nodiscard]] std::variant<std::vector<WorkRecord>, javelin::jmap::cache::DatabaseError>
        list() const override;
        [[nodiscard]] QString summary() const override;
        [[nodiscard]] QMetaObject::Connection
        connectChanged(QObject* context, std::function<void()> callback) override;

      private:
        GuiDaemonSession& m_session;
        RemoteActionClient& m_client;
    };
} // namespace javelin::app
