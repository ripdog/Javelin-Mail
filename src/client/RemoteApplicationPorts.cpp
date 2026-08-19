#include "client/RemoteApplicationPorts.h"

#include "app/RemoteActionTypes.h"
#include "client/GuiDaemonSession.h"

#include "jmap/OperationError.h"
#include "storage/DatabaseError.h"

#include <QDebug>
#include <QTimer>

#include <algorithm>
#include <ranges>
#include <type_traits>
#include <utility>

namespace javelin::app
{
    namespace
    {
        template <typename Variant, typename Alternative> struct VariantContains : std::false_type
        {
        };

        template <typename... Values, typename Alternative>
        struct VariantContains<std::variant<Values...>, Alternative>
            : std::bool_constant<(std::is_same_v<Values, Alternative> || ...)>
        {
        };

        [[nodiscard]] javelin::jmap::OperationError operationError(const RemoteCallError& error)
        {
            return {.code = javelin::jmap::OperationErrorCode::ProtocolViolation,
                    .message = error.detail};
        }

        [[nodiscard]] javelin::jmap::cache::DatabaseError
        databaseError(const RemoteCallError& error)
        {
            return {.code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                    .message = error.detail};
        }

        template <typename Result> [[nodiscard]] Result failureResult(const RemoteCallError& error)
        {
            if constexpr (std::is_same_v<Result, std::monostate>)
                return {};
            else if constexpr (std::is_same_v<Result, bool>)
                return false;
            else if constexpr (std::is_same_v<Result, QString>)
                return error.detail;
            else if constexpr (std::is_same_v<Result, RemoteUndoExecutionResult>)
                return {.succeeded = false,
                        .completedEntryId = std::nullopt,
                        .refreshScope = std::nullopt,
                        .failure = undo::HistoryFailure{.entryId = {},
                                                        .actionLabel = QStringLiteral("Undo/Redo"),
                                                        .summary = error.detail,
                                                        .objectFailures = {},
                                                        .mayRemoveFromHistory = false,
                                                        .acknowledgeAndRemove = false}};
            else if constexpr (std::is_same_v<Result, std::optional<javelin::jmap::OperationError>>)
                return operationError(error);
            else if constexpr (std::is_same_v<Result,
                                              std::optional<javelin::jmap::cache::DatabaseError>>)
                return databaseError(error);
            else if constexpr (VariantContains<Result, javelin::jmap::OperationError>::value)
                return Result{operationError(error)};
            else if constexpr (VariantContains<Result, javelin::jmap::cache::DatabaseError>::value)
                return Result{databaseError(error)};
            else if constexpr (VariantContains<Result, QString>::value)
                return Result{error.detail};
            else
                static_assert(sizeof(Result) == 0, "No remote failure representation for result");
        }

        template <typename Result> [[nodiscard]] Result unwrap(DecodedRemoteResult<Result> result)
        {
            if (auto* value = std::get_if<Result>(&result))
                return std::move(*value);
            return failureResult<Result>(std::get<RemoteCallError>(result));
        }

        template <typename Action, typename... Arguments>
        [[nodiscard]] QCoro::Task<typename Action::Result> call(RemoteActionClient& client,
                                                                Arguments&&... arguments)
        {
            auto result = co_await client.call<Action>(arguments...);
            co_return unwrap(std::move(result));
        }

        template <typename Action, typename... Arguments>
        [[nodiscard]] typename Action::Result callImmediate(RemoteActionClient& client,
                                                            Arguments&&... arguments)
        {
            return unwrap(client.callImmediate<Action>(arguments...));
        }

    } // namespace

    RemoteAccountCommandPort::RemoteAccountCommandPort(RemoteActionClient& client)
        : m_client(client)
    {
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    RemoteAccountCommandPort::removeConfiguredAccount(const QString& loginEmail,
                                                      const QString& sessionUrl,
                                                      const QStringList& knownAccountIds)
    {
        return callImmediate<javelin::protocol::actions::RemoveConfiguredAccount>(
            m_client, loginEmail, sessionUrl, knownAccountIds);
    }

    RemoteCalendarReader::RemoteCalendarReader(RemoteActionClient& client) : m_client(client)
    {
    }

    javelin::jmap::calendar::CalendarLoadResult RemoteCalendarReader::loadCached(
        const std::string_view accountId, const javelin::jmap::calendar::VisibleInterval& interval,
        const javelin::jmap::calendar::TimeZoneId& displayTimeZone) const
    {
        return callImmediate<javelin::protocol::actions::CalendarReadCached>(
            m_client, std::string{accountId}, interval, displayTimeZone);
    }

    javelin::jmap::calendar::CalendarAccountsResult RemoteCalendarReader::accounts() const
    {
        return callImmediate<javelin::protocol::actions::CalendarReadAccounts>(m_client);
    }

    javelin::jmap::calendar::CalendarListResult
    RemoteCalendarReader::calendars(const std::string_view accountId) const
    {
        return callImmediate<javelin::protocol::actions::CalendarReadCalendars>(
            m_client, std::string{accountId});
    }

    javelin::jmap::calendar::ParticipantIdentityListResult
    RemoteCalendarReader::participantIdentities(const std::string_view accountId) const
    {
        return callImmediate<javelin::protocol::actions::CalendarReadParticipantIdentities>(
            m_client, std::string{accountId});
    }

    javelin::jmap::calendar::PendingCalendarInvitationsResult
    RemoteCalendarReader::pendingInvitations() const
    {
        return callImmediate<javelin::protocol::actions::CalendarReadPendingInvitations>(m_client);
    }

    javelin::jmap::calendar::CalendarEventReadResult
    RemoteCalendarReader::event(const std::string_view accountId,
                                const std::string_view eventId) const
    {
        return callImmediate<javelin::protocol::actions::CalendarReadEvent>(
            m_client, std::string{accountId}, std::string{eventId});
    }

    RemoteCalendarCommandPort::RemoteCalendarCommandPort(RemoteActionClient& client,
                                                         QObject* parent)
        : CalendarCommandPort(parent), m_client(client)
    {
    }

    QCoro::Task<javelin::jmap::calendar::CalendarRefreshResult>
    RemoteCalendarCommandPort::requestCalendarRange(
        std::string ownerAccountId, javelin::jmap::calendar::VisibleInterval interval,
        javelin::jmap::calendar::TimeZoneId displayTimeZone)
    {
        co_return co_await call<javelin::protocol::actions::CalendarRequestRange>(
            m_client, ownerAccountId, interval, displayTimeZone);
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    RemoteCalendarCommandPort::createCalendarEvent(
        std::string ownerAccountId, javelin::jmap::calendar::CreateEventCommand command,
        const undo::CommandOrigin origin)
    {
        co_return co_await call<javelin::protocol::actions::CalendarCreateEvent>(
            m_client, ownerAccountId, command, origin);
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    RemoteCalendarCommandPort::updateCalendarEvent(
        std::string ownerAccountId, javelin::jmap::calendar::UpdateEventCommand command,
        const undo::CommandOrigin origin)
    {
        co_return co_await call<javelin::protocol::actions::CalendarUpdateEvent>(
            m_client, ownerAccountId, command, origin);
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    RemoteCalendarCommandPort::deleteCalendarEvent(
        std::string ownerAccountId, javelin::jmap::calendar::DeleteEventCommand command,
        const undo::CommandOrigin origin)
    {
        co_return co_await call<javelin::protocol::actions::CalendarDeleteEvent>(
            m_client, ownerAccountId, command, origin);
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    RemoteCalendarCommandPort::respondToCalendarEvent(
        std::string ownerAccountId, javelin::jmap::calendar::RespondToEventCommand command)
    {
        co_return co_await call<javelin::protocol::actions::CalendarRespondEvent>(
            m_client, ownerAccountId, command);
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    RemoteCalendarCommandPort::setCalendarSubscribed(std::string ownerAccountId,
                                                     std::string accountId, std::string calendarId,
                                                     const bool subscribed)
    {
        co_return co_await call<javelin::protocol::actions::CalendarSetSubscribed>(
            m_client, ownerAccountId, accountId, calendarId, subscribed);
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    RemoteCalendarCommandPort::setDefaultCalendar(std::string ownerAccountId, std::string accountId,
                                                  std::string calendarId,
                                                  const undo::CommandOrigin origin)
    {
        co_return co_await call<javelin::protocol::actions::CalendarSetDefault>(
            m_client, ownerAccountId, accountId, calendarId, origin);
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    RemoteCalendarCommandPort::createCalendar(
        std::string ownerAccountId, javelin::jmap::calendar::CreateCalendarCommand command)
    {
        co_return co_await call<javelin::protocol::actions::CalendarCreate>(
            m_client, ownerAccountId, command);
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    RemoteCalendarCommandPort::deleteCalendar(
        std::string ownerAccountId, javelin::jmap::calendar::DeleteCalendarCommand command)
    {
        co_return co_await call<javelin::protocol::actions::CalendarDelete>(
            m_client, ownerAccountId, command);
    }

    javelin::jmap::calendar::CalendarPreferenceResult
    RemoteCalendarCommandPort::setCalendarVisible(std::string accountId, std::string calendarId,
                                                  const bool visible,
                                                  const undo::CommandOrigin origin)
    {
        return callImmediate<javelin::protocol::actions::CalendarSetVisible>(
            m_client, accountId, calendarId, visible, origin);
    }

    RemoteComposeCommandPort::RemoteComposeCommandPort(RemoteActionClient& client)
        : m_client(client)
    {
    }

    QCoro::Task<
        std::variant<javelin::jmap::submission::DraftSnapshot, javelin::jmap::OperationError>>
    RemoteComposeCommandPort::open(AccountConnectionSettings settings,
                                   javelin::jmap::submission::OpenComposeRequest request)
    {
        return call<javelin::protocol::actions::ComposeOpen>(m_client, std::move(settings),
                                                             std::move(request));
    }

    QCoro::Task<
        std::variant<std::vector<javelin::jmap::domain::Identity>, javelin::jmap::OperationError>>
    RemoteComposeCommandPort::loadSenderIdentities(AccountConnectionSettings settings,
                                                   std::string accountId)
    {
        return call<javelin::protocol::actions::ComposeLoadSenderIdentities>(
            m_client, std::move(settings), std::move(accountId));
    }

    QCoro::Task<
        std::variant<javelin::jmap::submission::DraftSaveSummary, javelin::jmap::OperationError>>
    RemoteComposeCommandPort::saveDraft(AccountConnectionSettings settings,
                                        javelin::jmap::submission::DraftSnapshot snapshot)
    {
        return call<javelin::protocol::actions::ComposeSaveDraft>(m_client, std::move(settings),
                                                                  std::move(snapshot));
    }

    QCoro::Task<std::variant<javelin::jmap::submission::SendSummary, javelin::jmap::OperationError>>
    RemoteComposeCommandPort::send(AccountConnectionSettings settings,
                                   javelin::jmap::submission::DraftSnapshot snapshot)
    {
        return call<javelin::protocol::actions::ComposeSend>(m_client, std::move(settings),
                                                             std::move(snapshot));
    }

    QCoro::Task<std::variant<javelin::jmap::submission::SendSummary, javelin::jmap::OperationError>>
    RemoteComposeCommandPort::scheduleSend(AccountConnectionSettings settings,
                                           javelin::jmap::submission::ScheduledSendRequest request)
    {
        return call<javelin::protocol::actions::ComposeScheduleSend>(m_client, std::move(settings),
                                                                     std::move(request));
    }

    QCoro::Task<std::variant<bool, javelin::jmap::OperationError>>
    RemoteComposeCommandPort::cancelDeferredSend(QString sendId)
    {
        return call<javelin::protocol::actions::ComposeCancelDeferredSend>(m_client,
                                                                           std::move(sendId));
    }

    std::variant<std::optional<javelin::jmap::submission::DraftSnapshot>,
                 javelin::jmap::OperationError>
    RemoteComposeCommandPort::loadWorkingCopy(const std::string_view composeSessionId) const
    {
        return callImmediate<javelin::protocol::actions::ComposeLoadWorkingCopy>(
            m_client, std::string{composeSessionId});
    }

    std::optional<javelin::jmap::OperationError> RemoteComposeCommandPort::storeWorkingCopy(
        const javelin::jmap::submission::DraftSnapshot& snapshot)
    {
        return callImmediate<javelin::protocol::actions::ComposeStoreWorkingCopy>(m_client,
                                                                                  snapshot);
    }

    std::optional<javelin::jmap::OperationError>
    RemoteComposeCommandPort::discard(const std::string_view composeSessionId)
    {
        return callImmediate<javelin::protocol::actions::ComposeDiscard>(
            m_client, std::string{composeSessionId});
    }

    RemoteContactCommandPort::RemoteContactCommandPort(RemoteActionClient& client)
        : m_client(client)
    {
    }

#define JAVELIN_REMOTE_CONTACT_METHOD(methodName, commandType, actionType)                         \
    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>                                    \
    RemoteContactCommandPort::methodName(std::string ownerAccountId, commandType command)          \
    {                                                                                              \
        return call<javelin::protocol::actions::actionType>(m_client, std::move(ownerAccountId),   \
                                                            std::move(command));                   \
    }

    JAVELIN_REMOTE_CONTACT_METHOD(mutateAddressBook, AddressBookCommand, ContactMutateAddressBook)
    JAVELIN_REMOTE_CONTACT_METHOD(saveContact, SaveContactCommand, ContactSave)
    JAVELIN_REMOTE_CONTACT_METHOD(setContactsStarred, SetContactsStarredCommand, ContactSetStarred)
    JAVELIN_REMOTE_CONTACT_METHOD(deleteContacts, DeleteContactsCommand, ContactDelete)
    JAVELIN_REMOTE_CONTACT_METHOD(createContactGroup, CreateContactGroupCommand, ContactCreateGroup)
    JAVELIN_REMOTE_CONTACT_METHOD(deleteContactGroup, DeleteContactGroupCommand, ContactDeleteGroup)
    JAVELIN_REMOTE_CONTACT_METHOD(setContactGroupMembership, SetContactGroupMembershipCommand,
                                  ContactSetGroupMembership)
    JAVELIN_REMOTE_CONTACT_METHOD(copyContact, CopyContactCommand, ContactCopy)
    JAVELIN_REMOTE_CONTACT_METHOD(importContacts, ImportContactsCommand, ContactImport)
    JAVELIN_REMOTE_CONTACT_METHOD(mergeContacts, MergeContactsCommand, ContactMerge)

#undef JAVELIN_REMOTE_CONTACT_METHOD

    QCoro::Task<javelin::jmap::contacts::ContactUploadResult>
    RemoteContactCommandPort::uploadContactMedia(std::string ownerAccountId, std::string accountId,
                                                 QByteArray payload, std::string mediaType)
    {
        return call<javelin::protocol::actions::ContactUploadMedia>(
            m_client, std::move(ownerAccountId), std::move(accountId), std::move(payload),
            std::move(mediaType));
    }

    QCoro::Task<javelin::jmap::contacts::ContactDownloadResult>
    RemoteContactCommandPort::downloadContactMedia(std::string ownerAccountId,
                                                   std::string accountId, std::string blobId,
                                                   std::string mediaType)
    {
        return call<javelin::protocol::actions::ContactDownloadMedia>(
            m_client, std::move(ownerAccountId), std::move(accountId), std::move(blobId),
            std::move(mediaType));
    }

    RemoteMailCommandPort::RemoteMailCommandPort(RemoteActionClient& client) : m_client(client)
    {
    }

    QCoro::Task<QueuedMailboxSelectionMutationResult>
    RemoteMailCommandPort::queueMailboxSelectionMutation(MailboxSelectionMutationIntent intent)
    {
        return call<javelin::protocol::actions::MailQueueMailboxMutation>(m_client,
                                                                          std::move(intent));
    }

    QCoro::Task<MailTransferExecutionResult>
    RemoteMailCommandPort::transferAcrossAccounts(CrossAccountMailTransferIntent intent)
    {
        return call<javelin::protocol::actions::MailTransferAcrossAccounts>(m_client,
                                                                            std::move(intent));
    }

    QCoro::Task<QueuedMessageSelectionMutationResult>
    RemoteMailCommandPort::queueDestroyMessages(std::string accountId,
                                                std::optional<std::string> sourceMailboxId,
                                                MessageSelection selection)
    {
        return call<javelin::protocol::actions::MailQueueDestroy>(
            m_client, std::move(accountId), std::move(sourceMailboxId), std::move(selection));
    }

    QCoro::Task<QueuedMessageSelectionMutationResult>
    RemoteMailCommandPort::queueMarkMessagesUnread(std::string accountId,
                                                   std::optional<std::string> sourceMailboxId,
                                                   MessageSelection selection)
    {
        return call<javelin::protocol::actions::MailQueueMarkUnread>(
            m_client, std::move(accountId), std::move(sourceMailboxId), std::move(selection));
    }

    QCoro::Task<QueuedMessageSelectionMutationResult>
    RemoteMailCommandPort::queueMarkEmailRead(std::string accountId, std::string emailId)
    {
        return call<javelin::protocol::actions::MailQueueMarkRead>(m_client, std::move(accountId),
                                                                   std::move(emailId));
    }

    QCoro::Task<QueuedMessageSelectionMutationResult>
    RemoteMailCommandPort::queueSetMessagesFlagged(std::string accountId,
                                                   std::optional<std::string> sourceMailboxId,
                                                   MessageSelection selection, const bool flagged)
    {
        return call<javelin::protocol::actions::MailQueueSetSelectionFlagged>(
            m_client, std::move(accountId), std::move(sourceMailboxId), std::move(selection),
            flagged);
    }

    QCoro::Task<QueuedMessageSelectionMutationResult> RemoteMailCommandPort::queueSetMessagesTag(
        std::string accountId, std::optional<std::string> sourceMailboxId,
        MessageSelection selection, std::string keyword, const bool enabled)
    {
        return call<javelin::protocol::actions::MailQueueSetTag>(
            m_client, std::move(accountId), std::move(sourceMailboxId), std::move(selection),
            std::move(keyword), enabled);
    }

    QCoro::Task<SaveMailTagDefinitionResult>
    RemoteMailCommandPort::saveTagDefinition(SaveMailTagDefinition definition)
    {
        return call<javelin::protocol::actions::MailSaveTagDefinition>(m_client,
                                                                       std::move(definition));
    }

    QCoro::Task<QueuedMailTagDeletionResult> RemoteMailCommandPort::deleteTag(std::string accountId,
                                                                              std::string keyword)
    {
        return call<javelin::protocol::actions::MailDeleteTag>(m_client, std::move(accountId),
                                                               std::move(keyword));
    }

    QCoro::Task<javelin::jmap::MailboxSubscriptionChangeResult>
    RemoteMailCommandPort::setMailboxSubscribed(std::string accountId, std::string mailboxId,
                                                const bool subscribed)
    {
        return call<javelin::protocol::actions::MailSetMailboxSubscribed>(
            m_client, std::move(accountId), std::move(mailboxId), subscribed);
    }

    QCoro::Task<javelin::jmap::MailboxCreateResult>
    RemoteMailCommandPort::createMailbox(std::string accountId, std::string name)
    {
        return call<javelin::protocol::actions::MailCreateMailbox>(m_client, std::move(accountId),
                                                                   std::move(name));
    }

    QCoro::Task<javelin::jmap::MailboxDestroyResult>
    RemoteMailCommandPort::destroyMailbox(std::string accountId, std::string mailboxId)
    {
        return call<javelin::protocol::actions::MailDestroyMailbox>(m_client, std::move(accountId),
                                                                    std::move(mailboxId));
    }

    QCoro::Task<javelin::jmap::SubmittedEmailMutationsResult>
    RemoteMailCommandPort::submitPendingEmailMutations(std::string accountId,
                                                       std::optional<std::string> operationGroupId)
    {
        return call<javelin::protocol::actions::MailSubmitPending>(m_client, std::move(accountId),
                                                                   std::move(operationGroupId));
    }

    RemoteSieveCommandPort::RemoteSieveCommandPort(RemoteActionClient& client) : m_client(client)
    {
    }

    QCoro::Task<javelin::jmap::sieve::SieveListResult>
    RemoteSieveCommandPort::requestSieveScripts(std::string ownerAccountId)
    {
        return call<javelin::protocol::actions::SieveList>(m_client, std::move(ownerAccountId));
    }

    QCoro::Task<javelin::jmap::sieve::SieveContentResult>
    RemoteSieveCommandPort::requestSieveScript(std::string ownerAccountId,
                                               javelin::jmap::sieve::SieveScript script)
    {
        return call<javelin::protocol::actions::SieveGet>(m_client, std::move(ownerAccountId),
                                                          std::move(script));
    }

    QCoro::Task<javelin::jmap::sieve::SieveValidationResult>
    RemoteSieveCommandPort::validateSieveScript(std::string ownerAccountId, QByteArray content)
    {
        return call<javelin::protocol::actions::SieveValidate>(m_client, std::move(ownerAccountId),
                                                               std::move(content));
    }

    QCoro::Task<javelin::jmap::sieve::SieveSaveResult>
    RemoteSieveCommandPort::saveSieveScript(std::string ownerAccountId,
                                            javelin::jmap::sieve::SieveScript script,
                                            QByteArray content, const undo::CommandOrigin origin)
    {
        return call<javelin::protocol::actions::SieveSave>(
            m_client, std::move(ownerAccountId), std::move(script), std::move(content), origin);
    }

    QCoro::Task<javelin::jmap::sieve::SieveDeleteResult>
    RemoteSieveCommandPort::deleteSieveScript(std::string ownerAccountId,
                                              javelin::jmap::sieve::SieveScript script,
                                              const undo::CommandOrigin origin)
    {
        return call<javelin::protocol::actions::SieveDelete>(m_client, std::move(ownerAccountId),
                                                             std::move(script), origin);
    }

    QCoro::Task<javelin::jmap::sieve::SieveActivationResult>
    RemoteSieveCommandPort::setSieveScriptActive(std::string ownerAccountId,
                                                 javelin::jmap::sieve::SieveScript script,
                                                 const bool active,
                                                 const undo::CommandOrigin origin)
    {
        return call<javelin::protocol::actions::SieveActivate>(m_client, std::move(ownerAccountId),
                                                               std::move(script), active, origin);
    }

    RemoteIdentityCommandPort::RemoteIdentityCommandPort(RemoteActionClient& client)
        : m_client(client)
    {
    }

    QCoro::Task<javelin::jmap::identity::IdentityListResult>
    RemoteIdentityCommandPort::requestSenderIdentities(std::string accountId)
    {
        return call<javelin::protocol::actions::IdentityList>(m_client, std::move(accountId));
    }

    QCoro::Task<javelin::jmap::identity::IdentitySaveResult>
    RemoteIdentityCommandPort::saveSenderIdentity(std::string accountId,
                                                  javelin::jmap::domain::Identity identity)
    {
        return call<javelin::protocol::actions::IdentitySave>(m_client, std::move(accountId),
                                                              std::move(identity));
    }

    QCoro::Task<javelin::jmap::identity::IdentityDeleteResult>
    RemoteIdentityCommandPort::deleteSenderIdentity(std::string accountId, std::string identityId)
    {
        return call<javelin::protocol::actions::IdentityDelete>(m_client, std::move(accountId),
                                                                std::move(identityId));
    }

    RemoteAccountRefreshPort::RemoteAccountRefreshPort(GuiDaemonSession& session,
                                                       RemoteActionClient& client)
        : m_session(session), m_client(client)
    {
    }

    bool RemoteAccountRefreshPort::requestAccountSynchronization(const std::string_view accountId)
    {
        return !m_session.requestAccountRefresh(QString::fromUtf8(accountId)).has_value();
    }

    QCoro::Task<javelin::jmap::LiveRefreshResult>
    RemoteAccountRefreshPort::bootstrapAccount(AccountBootstrapIntent intent)
    {
        return call<javelin::protocol::actions::AccountBootstrap>(m_client, std::move(intent));
    }

    QCoro::Task<javelin::jmap::contacts::ContactRefreshResult>
    RemoteAccountRefreshPort::requestContacts(std::string ownerAccountId)
    {
        return call<javelin::protocol::actions::ContactRequestRefresh>(m_client,
                                                                       std::move(ownerAccountId));
    }

    RemoteMessageListMaterializationPort::RemoteMessageListMaterializationPort(
        RemoteActionClient& client)
        : m_client(client)
    {
    }

    MailboxObservationLease
    RemoteMessageListMaterializationPort::beginMailboxObservation(std::string accountId,
                                                                  std::string mailboxId)
    {
        const auto observationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto observe = m_client.callDiscardingResult<javelin::protocol::actions::MailboxObserve>(
            observationId, std::move(accountId), std::move(mailboxId));
        QCoro::connect(std::move(observe), &m_client, [](bool) {});
        return MailboxObservationLease{
            [this, observationId]
            {
                auto unobserve =
                    m_client.callDiscardingResult<javelin::protocol::actions::MailboxUnobserve>(
                        observationId);
                QCoro::connect(std::move(unobserve), &m_client, [](bool) {});
            }};
    }

    QCoro::Task<MailboxWindowResult>
    RemoteMessageListMaterializationPort::requestMailboxWindow(MailboxWindowIntent intent)
    {
        return call<javelin::protocol::actions::MailboxWindow>(m_client, std::move(intent));
    }

    QCoro::Task<SearchWindowResult>
    RemoteMessageListMaterializationPort::requestSearchWindow(SearchWindowIntent intent)
    {
        return call<javelin::protocol::actions::SearchWindow>(m_client, std::move(intent));
    }

    void RemoteMessageListMaterializationPort::ensureThread(ThreadMaterializationIntent intent)
    {
        auto task = m_client.callDiscardingResult<javelin::protocol::actions::ThreadEnsure>(
            std::move(intent));
        QCoro::connect(std::move(task), &m_client, [](bool) {});
    }

    void RemoteMessageListMaterializationPort::retireSearchWindow(std::string accountId,
                                                                  std::string windowKey)
    {
        auto task = m_client.callDiscardingResult<javelin::protocol::actions::SearchRetire>(
            std::move(accountId), std::move(windowKey));
        QCoro::connect(std::move(task), &m_client, [](bool) {});
    }

    RemoteMailExportPort::RemoteMailExportPort(RemoteActionClient& client) : m_client(client)
    {
    }

    QCoro::Task<MailExportStartResult> RemoteMailExportPort::startExport(MailExportIntent intent)
    {
        return call<javelin::protocol::actions::StartMailExport>(m_client, std::move(intent));
    }

    RemoteMessageContentPort::RemoteMessageContentPort(RemoteActionClient& client)
        : m_client(client)
    {
    }

    QCoro::Task<javelin::jmap::MessageContentRefreshResult>
    RemoteMessageContentPort::requestMessageContent(std::string accountId, std::string emailId)
    {
        return call<javelin::protocol::actions::MessageContent>(m_client, std::move(accountId),
                                                                std::move(emailId));
    }

    QCoro::Task<javelin::jmap::AttachmentDownloadResult>
    RemoteMessageContentPort::requestAttachment(std::string accountId, std::string emailId,
                                                std::string partId)
    {
        return call<javelin::protocol::actions::AttachmentDownload>(
            m_client, std::move(accountId), std::move(emailId), std::move(partId));
    }

    QCoro::Task<javelin::jmap::MessageSourceDownloadResult>
    RemoteMessageContentPort::requestMessageSource(std::string accountId, std::string emailId)
    {
        return call<javelin::protocol::actions::MessageSource>(m_client, std::move(accountId),
                                                               std::move(emailId));
    }

    QCoro::Task<SaveMessagesResult>
    RemoteMessageContentPort::saveMessages(SaveMessagesIntent intent)
    {
        return call<javelin::protocol::actions::SaveMessages>(m_client, std::move(intent));
    }

    RemoteUndoCommandPort::RemoteUndoCommandPort(GuiDaemonSession& session,
                                                 RemoteActionClient& client, QObject* parent)
        : UndoCommandPort(parent), m_session(session), m_client(client)
    {
        refreshSnapshot();
        connect(&m_session, &GuiDaemonSession::cacheInvalidated, this,
                [this](const javelin::protocol::CacheInvalidation& invalidation)
                {
                    if (std::ranges::contains(invalidation.changedDomains,
                                              javelin::protocol::ChangedDomain::History))
                        refreshSnapshot();
                });
    }

    QCoro::Task<bool> RemoteUndoCommandPort::undo()
    {
        auto result = co_await call<javelin::protocol::actions::Undo>(m_client);
        refreshSnapshot();
        if (result.failure.has_value())
            Q_EMIT executionFailed(*result.failure);
        else if (result.completedEntryId.has_value() && result.refreshScope.has_value())
            Q_EMIT executionCompleted(*result.completedEntryId, *result.refreshScope);
        co_return result.succeeded;
    }

    QCoro::Task<bool> RemoteUndoCommandPort::redo()
    {
        auto result = co_await call<javelin::protocol::actions::Redo>(m_client);
        refreshSnapshot();
        if (result.failure.has_value())
            Q_EMIT executionFailed(*result.failure);
        else if (result.completedEntryId.has_value() && result.refreshScope.has_value())
            Q_EMIT executionCompleted(*result.completedEntryId, *result.refreshScope);
        co_return result.succeeded;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    RemoteUndoCommandPort::acknowledgeAndRemove(const QString& entryId)
    {
        auto result =
            callImmediate<javelin::protocol::actions::UndoAcknowledgeRemove>(m_client, entryId);
        refreshSnapshot();
        return result;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    RemoteUndoCommandPort::forget(const QString& entryId)
    {
        auto result = callImmediate<javelin::protocol::actions::UndoForget>(m_client, entryId);
        refreshSnapshot();
        return result;
    }

    const undo::HistoryState& RemoteUndoCommandPort::state() const
    {
        return m_state;
    }

    const std::vector<undo::HistoryEntry>& RemoteUndoCommandPort::entries() const
    {
        return m_entries;
    }

    void RemoteUndoCommandPort::refreshSnapshot()
    {
        using Snapshot = std::tuple<undo::HistoryState, std::vector<undo::HistoryEntry>>;
        auto result = m_client.callImmediate<javelin::protocol::actions::UndoSnapshot>();
        if (const auto* error = std::get_if<RemoteCallError>(&result))
        {
            Q_EMIT executionFailed({.entryId = {},
                                    .actionLabel = QStringLiteral("History"),
                                    .summary = error->detail,
                                    .objectFailures = {},
                                    .mayRemoveFromHistory = false,
                                    .acknowledgeAndRemove = false});
            return;
        }
        auto [stateValue, entriesValue] = std::get<Snapshot>(std::move(result));
        m_state = std::move(stateValue);
        m_entries = std::move(entriesValue);
        Q_EMIT historyStateChanged(m_state);
    }

    RemoteDeveloperDiagnosticsPort::RemoteDeveloperDiagnosticsPort(RemoteActionClient& client)
        : m_client(client)
    {
    }

    QCoro::Task<DeveloperDiagnosticsResult> RemoteDeveloperDiagnosticsPort::snapshot()
    {
        co_return co_await call<javelin::protocol::actions::DeveloperDiagnosticsSnapshot>(m_client);
    }

    RemoteDeveloperMaintenancePort::RemoteDeveloperMaintenancePort(RemoteActionClient& client)
        : m_client(client)
    {
    }

    QCoro::Task<DeveloperMailboxClearResult>
    RemoteDeveloperMaintenancePort::clearMailboxCache(DeveloperMailboxClearCommand command)
    {
        co_return co_await call<javelin::protocol::actions::DeveloperMailboxClear>(
            m_client, std::move(command));
    }

    RemoteDaemonLogPort::RemoteDaemonLogPort(GuiDaemonSession& session, RemoteActionClient& client,
                                             QObject* parent)
        : DaemonLogPort(parent), m_session(session), m_client(client)
    {
        connect(&m_session, &GuiDaemonSession::daemonLogEntryAdded, this,
                [this](const LogEntry& entry)
                {
                    if (m_subscribers == 0)
                        return;
                    m_entries.push_back(entry);
                    if (m_entries.size() > 1000)
                        m_entries.remove(0, m_entries.size() - 1000);
                    Q_EMIT entryAdded(entry);
                });
        connect(&m_session, &GuiDaemonSession::ready, this,
                [this]
                {
                    if (m_subscribers == 0)
                        return;
                    QTimer::singleShot(0, this,
                                       [this]
                                       {
                                           if (m_subscribers == 0)
                                               return;
                                           m_entries.clear();
                                           Q_EMIT cleared();
                                           setRemoteSubscribed(true);
                                       });
                });
    }

    QVector<LogEntry> RemoteDaemonLogPort::entries() const
    {
        return m_entries;
    }

    void RemoteDaemonLogPort::acquire()
    {
        ++m_subscribers;
        if (m_subscribers != 1)
            return;
        m_entries.clear();
        setRemoteSubscribed(true);
    }

    void RemoteDaemonLogPort::release()
    {
        if (m_subscribers == 0)
            return;
        --m_subscribers;
        if (m_subscribers != 0)
            return;
        setRemoteSubscribed(false);
        m_entries.clear();
    }

    void RemoteDaemonLogPort::clear()
    {
        const auto result = m_client.callImmediate<javelin::protocol::actions::DeveloperLogClear>();
        if (const auto* error = std::get_if<RemoteCallError>(&result))
            qWarning().noquote() << "Clear daemon log failed" << error->detail;
        m_entries.clear();
        Q_EMIT cleared();
    }

    void RemoteDaemonLogPort::setRemoteSubscribed(const bool subscribed)
    {
        const auto result =
            m_client.callImmediate<javelin::protocol::actions::DeveloperLogSetSubscribed>(
                subscribed);
        if (const auto* error = std::get_if<RemoteCallError>(&result))
            qWarning().noquote() << "Daemon log subscription failed" << error->detail;
    }

    RemoteWorkTaskPort::RemoteWorkTaskPort(GuiDaemonSession& session, RemoteActionClient& client)
        : m_session(session), m_client(client)
    {
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    RemoteWorkTaskPort::pause(const std::string_view jobId)
    {
        return callImmediate<javelin::protocol::actions::WorkPause>(m_client, std::string{jobId});
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    RemoteWorkTaskPort::resume(const std::string_view jobId)
    {
        return callImmediate<javelin::protocol::actions::WorkResume>(m_client, std::string{jobId});
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    RemoteWorkTaskPort::retry(const std::string_view jobId)
    {
        return callImmediate<javelin::protocol::actions::WorkRetry>(m_client, std::string{jobId});
    }

    std::variant<std::vector<WorkRecord>, javelin::jmap::cache::DatabaseError>
    RemoteWorkTaskPort::list() const
    {
        return callImmediate<javelin::protocol::actions::WorkList>(m_client);
    }

    QString RemoteWorkTaskPort::summary() const
    {
        return callImmediate<javelin::protocol::actions::WorkSummary>(m_client);
    }

    QMetaObject::Connection RemoteWorkTaskPort::connectChanged(QObject* context,
                                                               std::function<void()> callback)
    {
        return QObject::connect(
            &m_session, &GuiDaemonSession::cacheInvalidated, context,
            [callback =
                 std::move(callback)](const javelin::protocol::CacheInvalidation& invalidation)
            {
                if (std::ranges::contains(invalidation.changedDomains,
                                          javelin::protocol::ChangedDomain::BackgroundJobs))
                    callback();
            });
    }
} // namespace javelin::app
