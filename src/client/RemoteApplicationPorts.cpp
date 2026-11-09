#include "client/RemoteApplicationPorts.h"

#include "app/RemoteActionTypes.h"
#include "client/GuiDaemonSession.h"

#include "jmap/OperationError.h"
#include "storage/DatabaseError.h"

#include <QDebug>
#include <QPointer>
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
            else if constexpr (std::is_same_v<Result, CalendarColorBatchResult> ||
                               std::is_same_v<Result, CalendarDefaultAlertsBatchResult>)
                return {.requestedCount = 0,
                        .appliedCount = 0,
                        .failures = {},
                        .error = operationError(error)};
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

    } // namespace

    RemoteAccountCommandPort::RemoteAccountCommandPort(RemoteActionClient& client)
        : m_client(client)
    {
    }

    QCoro::Task<std::optional<javelin::jmap::cache::DatabaseError>>
    RemoteAccountCommandPort::removeConfiguredAccount(QString loginEmail, QString sessionUrl,
                                                      QStringList knownAccountIds)
    {
        return call<javelin::protocol::actions::RemoveConfiguredAccount>(
            m_client, std::move(loginEmail), std::move(sessionUrl), std::move(knownAccountIds));
    }

    RemoteCalendarReader::RemoteCalendarReader(RemoteActionClient& client) : m_client(client)
    {
    }

    QCoro::Task<javelin::jmap::calendar::CalendarLoadResult>
    RemoteCalendarReader::loadCached(std::string accountId,
                                     javelin::jmap::calendar::VisibleInterval interval,
                                     javelin::jmap::calendar::TimeZoneId displayTimeZone)
    {
        return call<javelin::protocol::actions::CalendarReadCached>(
            m_client, std::move(accountId), std::move(interval), std::move(displayTimeZone));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarAccountsResult> RemoteCalendarReader::accounts()
    {
        return call<javelin::protocol::actions::CalendarReadAccounts>(m_client);
    }

    QCoro::Task<javelin::jmap::calendar::CalendarListResult>
    RemoteCalendarReader::calendars(std::string accountId)
    {
        return call<javelin::protocol::actions::CalendarReadCalendars>(m_client,
                                                                       std::move(accountId));
    }

    QCoro::Task<javelin::jmap::calendar::ParticipantIdentityListResult>
    RemoteCalendarReader::participantIdentities(std::string accountId)
    {
        return call<javelin::protocol::actions::CalendarReadParticipantIdentities>(
            m_client, std::move(accountId));
    }

    QCoro::Task<javelin::jmap::calendar::PendingCalendarInvitationsResult>
    RemoteCalendarReader::pendingInvitations()
    {
        return call<javelin::protocol::actions::CalendarReadPendingInvitations>(m_client);
    }

    RemoteCalendarCommandPort::RemoteCalendarCommandPort(RemoteActionClient& client,
                                                         QObject* parent)
        : CalendarCommandPort(parent), m_client(client)
    {
    }

    QCoro::Task<javelin::jmap::calendar::CalendarRefreshResult>
    RemoteCalendarCommandPort::requestCalendarRange(
        std::string ownerAccountId, javelin::jmap::calendar::VisibleInterval interval,
        javelin::jmap::calendar::TimeZoneId displayTimeZone, const bool forceRefresh)
    {
        co_return co_await call<javelin::protocol::actions::CalendarRequestRange>(
            m_client, ownerAccountId, interval, displayTimeZone, forceRefresh);
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
    RemoteCalendarCommandPort::setCalendarColor(std::string ownerAccountId, std::string accountId,
                                                std::string calendarId,
                                                std::optional<std::string> color)
    {
        co_return co_await call<javelin::protocol::actions::CalendarSetColor>(
            m_client, ownerAccountId, accountId, calendarId, color);
    }

    QCoro::Task<CalendarColorBatchResult>
    RemoteCalendarCommandPort::setCalendarColors(std::vector<CalendarColorChange> changes)
    {
        const auto requestedCount = changes.size();
        auto result =
            co_await call<javelin::protocol::actions::CalendarSetColors>(m_client, changes);
        if (result.error.has_value() && result.requestedCount == 0 && requestedCount > 0)
        {
            result.requestedCount = requestedCount;
            result.failures.reserve(changes.size());
            for (const auto& change : changes)
                result.failures.push_back({.change = change, .error = *result.error});
        }
        co_return result;
    }

    QCoro::Task<CalendarDefaultAlertsBatchResult>
    RemoteCalendarCommandPort::setCalendarDefaultAlerts(
        std::vector<CalendarDefaultAlertsChange> changes)
    {
        const auto requestedCount = changes.size();
        auto result =
            co_await call<javelin::protocol::actions::CalendarSetDefaultAlerts>(m_client, changes);
        if (result.error.has_value() && result.requestedCount == 0 && requestedCount > 0)
        {
            result.requestedCount = requestedCount;
            result.failures.reserve(changes.size());
            for (const auto& change : changes)
                result.failures.push_back({.change = change, .error = *result.error});
        }
        co_return result;
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

    QCoro::Task<javelin::jmap::calendar::CalendarPreferenceResult>
    RemoteCalendarCommandPort::setCalendarVisible(std::string accountId, std::string calendarId,
                                                  const bool visible,
                                                  const undo::CommandOrigin origin)
    {
        return call<javelin::protocol::actions::CalendarSetVisible>(
            m_client, std::move(accountId), std::move(calendarId), visible, origin);
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

    QCoro::Task<std::variant<std::optional<javelin::jmap::submission::DraftSnapshot>,
                             javelin::jmap::OperationError>>
    RemoteComposeCommandPort::loadWorkingCopy(std::string composeSessionId)
    {
        return call<javelin::protocol::actions::ComposeLoadWorkingCopy>(
            m_client, std::move(composeSessionId));
    }

    QCoro::Task<std::optional<javelin::jmap::OperationError>>
    RemoteComposeCommandPort::storeWorkingCopy(javelin::jmap::submission::DraftSnapshot snapshot)
    {
        return call<javelin::protocol::actions::ComposeStoreWorkingCopy>(m_client,
                                                                         std::move(snapshot));
    }

    QCoro::Task<std::optional<javelin::jmap::OperationError>>
    RemoteComposeCommandPort::discard(std::string composeSessionId)
    {
        return call<javelin::protocol::actions::ComposeDiscard>(m_client,
                                                                std::move(composeSessionId));
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

    RemoteMailImportPort::RemoteMailImportPort(RemoteActionClient& client) : m_client(client)
    {
    }

    QCoro::Task<MailImportStartResult> RemoteMailImportPort::startImport(MailImportIntent intent)
    {
        return call<javelin::protocol::actions::StartMailImport>(m_client, std::move(intent));
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
        QCoro::connect(refreshSnapshot(), this, []() {});
        connect(&m_session, &GuiDaemonSession::cacheInvalidated, this,
                [this](const javelin::protocol::CacheInvalidation& invalidation)
                {
                    if (std::ranges::contains(invalidation.changedDomains,
                                              javelin::protocol::ChangedDomain::History))
                        QCoro::connect(refreshSnapshot(), this, []() {});
                });
    }

    QCoro::Task<bool> RemoteUndoCommandPort::undo()
    {
        const QPointer<RemoteUndoCommandPort> self{this};
        auto result = co_await call<javelin::protocol::actions::Undo>(m_client);
        if (!self)
            co_return result.succeeded;
        co_await refreshSnapshot();
        if (!self)
            co_return result.succeeded;
        if (result.failure.has_value())
            Q_EMIT executionFailed(*result.failure);
        else if (result.completedEntryId.has_value() && result.refreshScope.has_value())
            Q_EMIT executionCompleted(*result.completedEntryId, *result.refreshScope);
        co_return result.succeeded;
    }

    QCoro::Task<bool> RemoteUndoCommandPort::redo()
    {
        const QPointer<RemoteUndoCommandPort> self{this};
        auto result = co_await call<javelin::protocol::actions::Redo>(m_client);
        if (!self)
            co_return result.succeeded;
        co_await refreshSnapshot();
        if (!self)
            co_return result.succeeded;
        if (result.failure.has_value())
            Q_EMIT executionFailed(*result.failure);
        else if (result.completedEntryId.has_value() && result.refreshScope.has_value())
            Q_EMIT executionCompleted(*result.completedEntryId, *result.refreshScope);
        co_return result.succeeded;
    }

    QCoro::Task<std::optional<javelin::jmap::cache::DatabaseError>>
    RemoteUndoCommandPort::acknowledgeAndRemove(QString entryId)
    {
        const QPointer<RemoteUndoCommandPort> self{this};
        auto result = co_await call<javelin::protocol::actions::UndoAcknowledgeRemove>(
            m_client, std::move(entryId));
        if (self)
            co_await refreshSnapshot();
        co_return result;
    }

    QCoro::Task<std::optional<javelin::jmap::cache::DatabaseError>>
    RemoteUndoCommandPort::forget(QString entryId)
    {
        const QPointer<RemoteUndoCommandPort> self{this};
        auto result =
            co_await call<javelin::protocol::actions::UndoForget>(m_client, std::move(entryId));
        if (self)
            co_await refreshSnapshot();
        co_return result;
    }

    const undo::HistoryState& RemoteUndoCommandPort::state() const
    {
        return m_state;
    }

    const std::vector<undo::HistoryEntry>& RemoteUndoCommandPort::entries() const
    {
        return m_entries;
    }

    QCoro::Task<> RemoteUndoCommandPort::refreshSnapshot()
    {
        using Snapshot = std::tuple<undo::HistoryState, std::vector<undo::HistoryEntry>>;
        const QPointer<RemoteUndoCommandPort> self{this};
        const auto request = ++m_requestedSnapshot;
        auto result = co_await m_client.call<javelin::protocol::actions::UndoSnapshot>();
        // Snapshots requested later reflect at least as much history; never let an older reply
        // overwrite one that was already applied.
        if (!self || request <= m_appliedSnapshot)
            co_return;
        if (const auto* error = std::get_if<RemoteCallError>(&result))
        {
            Q_EMIT executionFailed({.entryId = {},
                                    .actionLabel = QStringLiteral("History"),
                                    .summary = error->detail,
                                    .objectFailures = {},
                                    .mayRemoveFromHistory = false,
                                    .acknowledgeAndRemove = false});
            co_return;
        }
        m_appliedSnapshot = request;
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
                    // A new daemon connection starts without a log subscription.
                    ++m_connectionEpoch;
                    m_remoteSubscribed = false;
                    if (m_subscribers == 0)
                        return;
                    QTimer::singleShot(0, this,
                                       [this]
                                       {
                                           if (m_subscribers == 0)
                                               return;
                                           m_entries.clear();
                                           Q_EMIT cleared();
                                           QCoro::connect(synchronizeRemoteSubscription(), this,
                                                          []() {});
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
        QCoro::connect(synchronizeRemoteSubscription(), this, []() {});
    }

    void RemoteDaemonLogPort::release()
    {
        if (m_subscribers == 0)
            return;
        --m_subscribers;
        if (m_subscribers != 0)
            return;
        m_entries.clear();
        QCoro::connect(synchronizeRemoteSubscription(), this, []() {});
    }

    void RemoteDaemonLogPort::clear()
    {
        QCoro::connect(clearRemote(), this, []() {});
    }

    QCoro::Task<> RemoteDaemonLogPort::clearRemote()
    {
        const QPointer<RemoteDaemonLogPort> self{this};
        const auto result = co_await m_client.call<javelin::protocol::actions::DeveloperLogClear>();
        if (!self)
            co_return;
        if (const auto* error = std::get_if<RemoteCallError>(&result))
            qWarning().noquote() << "Clear daemon log failed" << error->detail;
        // Log events precede the clear reply on the connection, so entries buffered here are
        // exactly the ones the daemon discarded.
        m_entries.clear();
        Q_EMIT cleared();
    }

    QCoro::Task<> RemoteDaemonLogPort::synchronizeRemoteSubscription()
    {
        if (m_subscriptionInFlight)
            co_return;
        const QPointer<RemoteDaemonLogPort> self{this};
        m_subscriptionInFlight = true;
        while (m_remoteSubscribed != (m_subscribers != 0))
        {
            const bool subscribed = m_subscribers != 0;
            const auto epoch = m_connectionEpoch;
            const auto result =
                co_await m_client.call<javelin::protocol::actions::DeveloperLogSetSubscribed>(
                    subscribed);
            if (!self)
                co_return;
            if (const auto* error = std::get_if<RemoteCallError>(&result))
                qWarning().noquote() << "Daemon log subscription failed" << error->detail;
            // A failed request is not retried until the subscription is next requested.
            if (epoch == m_connectionEpoch)
                m_remoteSubscribed = subscribed;
        }
        m_subscriptionInFlight = false;
    }

    RemoteWorkTaskPort::RemoteWorkTaskPort(GuiDaemonSession& session, RemoteActionClient& client)
        : m_session(session), m_client(client)
    {
    }

    QCoro::Task<std::optional<javelin::jmap::cache::DatabaseError>>
    RemoteWorkTaskPort::pause(std::string jobId)
    {
        return call<javelin::protocol::actions::WorkPause>(m_client, std::move(jobId));
    }

    QCoro::Task<std::optional<javelin::jmap::cache::DatabaseError>>
    RemoteWorkTaskPort::resume(std::string jobId)
    {
        return call<javelin::protocol::actions::WorkResume>(m_client, std::move(jobId));
    }

    QCoro::Task<std::optional<javelin::jmap::cache::DatabaseError>>
    RemoteWorkTaskPort::retry(std::string jobId)
    {
        return call<javelin::protocol::actions::WorkRetry>(m_client, std::move(jobId));
    }

    QCoro::Task<std::variant<std::vector<WorkRecord>, javelin::jmap::cache::DatabaseError>>
    RemoteWorkTaskPort::list()
    {
        return call<javelin::protocol::actions::WorkList>(m_client);
    }

    QCoro::Task<QString> RemoteWorkTaskPort::summary()
    {
        return call<javelin::protocol::actions::WorkSummary>(m_client);
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
