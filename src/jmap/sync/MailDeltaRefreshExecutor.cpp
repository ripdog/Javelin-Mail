#include "jmap/sync/MailDeltaRefreshExecutor.h"

#include "jmap/api/MailMethods.h"
#include "jmap/api/ResponseReader.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/SearchWindowRepository.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/sync/ConsistencyDomain.h"
#include "jmap/sync/MailboxRefreshExecutor.h"
#include "jmap/sync/MutationJournal.h"

#include <algorithm>
#include <optional>
#include <unordered_set>

namespace javelin::jmap::sync
{

    namespace
    {
        using javelin::jmap::api::CallHandle;
        using javelin::jmap::api::EmailChangesResponse;
        using javelin::jmap::api::EmailGetResponse;
        using javelin::jmap::api::MailboxChangesResponse;
        using javelin::jmap::api::MailboxGetResponse;

        struct DeltaHandles
        {
            std::optional<CallHandle<MailboxChangesResponse>> mailboxChanges;
            std::optional<CallHandle<MailboxGetResponse>> createdMailboxes;
            std::optional<CallHandle<MailboxGetResponse>> updatedMailboxes;
            std::optional<CallHandle<EmailChangesResponse>> emailChanges;
            std::optional<CallHandle<EmailGetResponse>> createdEmails;
            std::optional<CallHandle<EmailGetResponse>> updatedEmails;
        };

        struct ParsedDelta
        {
            std::optional<MailboxChangesResponse> mailboxChanges;
            std::vector<javelin::jmap::domain::Mailbox> mailboxes;
            std::vector<std::string> lateDestroyedMailboxes;
            bool mailboxNeedsContinuation = false;
            std::optional<EmailChangesResponse> emailChanges;
            std::vector<javelin::jmap::domain::Email> emails;
            std::vector<std::string> lateDestroyedEmails;
            bool emailNeedsContinuation = false;
        };

        struct MaterializationAssessment
        {
            bool complete = true;
            bool needsContinuation = false;
            std::vector<std::string> lateDestroyed;
        };

        [[nodiscard]] javelin::jmap::cache::SyncStateKey syncKey(const std::string_view accountId,
                                                                 const std::string_view type)
        {
            return {
                .accountId = std::string{accountId},
                .objectType = std::string{type},
                .queryKey = {},
            };
        }

        template <typename T> void appendUnique(std::vector<T>& destination, const T& value)
        {
            if (std::ranges::find(destination, value) == destination.end())
                destination.push_back(value);
        }

        template <typename T>
        void appendUnique(std::vector<T>& destination, const std::vector<T>& values)
        {
            for (const auto& value : values)
                appendUnique(destination, value);
        }

        template <typename Object>
        [[nodiscard]] MaterializationAssessment
        assessMaterialization(const std::vector<std::string>& requestedIds,
                              const std::vector<Object>& returned,
                              const std::vector<std::string>& notFound,
                              const std::string_view changesState, const std::string_view getState)
        {
            MaterializationAssessment assessment{
                .complete = true,
                .needsContinuation = getState != changesState,
                .lateDestroyed = notFound,
            };
            std::unordered_set<std::string> requested(requestedIds.begin(), requestedIds.end());
            std::unordered_set<std::string> accounted(notFound.begin(), notFound.end());
            for (const auto& object : returned)
            {
                if (!requested.contains(object.id) || !accounted.insert(object.id).second)
                {
                    assessment.complete = false;
                    continue;
                }
            }
            for (const auto& id : notFound)
            {
                if (!requested.contains(id))
                    assessment.complete = false;
            }
            for (const auto& id : requestedIds)
            {
                if (!accounted.contains(id))
                    assessment.complete = false;
            }
            if (!notFound.empty())
                assessment.needsContinuation = true;
            return assessment;
        }

        [[nodiscard]] bool sameSet(std::vector<std::string> left, std::vector<std::string> right)
        {
            std::ranges::sort(left);
            std::ranges::sort(right);
            return left == right;
        }

        [[nodiscard]] bool
        isRecoverableChangesError(const javelin::jmap::api::ResponseReaderError& error)
        {
            return error.code == javelin::jmap::api::ResponseReaderErrorCode::MethodError &&
                   error.methodError.has_value() &&
                   (error.methodError->type == "cannotCalculateChanges" ||
                    error.methodError->type == "tooManyChanges");
        }

        template <typename Response>
        [[nodiscard]] std::optional<OperationError>
        readRequired(const javelin::jmap::api::ResponseReader& reader,
                     const CallHandle<Response>& handle, Response& response)
        {
            const auto result = reader.require(handle);
            if (const auto* error = std::get_if<javelin::jmap::api::ResponseReaderError>(&result))
                return operationError(*error);
            response = std::get<Response>(result);
            return std::nullopt;
        }

        [[nodiscard]] std::optional<OperationError>
        addMailboxDeltaCalls(javelin::jmap::api::RequestBuilder& builder,
                             const std::string_view accountId, const std::string_view sinceState,
                             const std::optional<std::uint64_t> maxChanges, DeltaHandles& handles)
        {
            const auto changes = javelin::jmap::api::mailboxChanges({
                .accountId = std::string{accountId},
                .sinceState = std::string{sinceState},
                .maxChanges = maxChanges,
            });
            if (!changes.has_value())
                return OperationError{
                    .message = QStringLiteral("Failed to encode Mailbox/changes."),
                };
            handles.mailboxChanges = builder.call(*changes, "mailbox-changes");

            const auto created = javelin::jmap::api::mailboxGet(javelin::jmap::api::getRequestFrom(
                std::string{accountId}, *handles.mailboxChanges, "/created"));
            const auto updated = javelin::jmap::api::mailboxGet(javelin::jmap::api::getRequestFrom(
                std::string{accountId}, *handles.mailboxChanges, "/updated"));
            if (!created.has_value() || !updated.has_value())
                return OperationError{
                    .message = QStringLiteral("Failed to encode delta Mailbox/get."),
                };
            handles.createdMailboxes = builder.call(*created, "created-mailboxes");
            handles.updatedMailboxes = builder.call(*updated, "updated-mailboxes");
            return std::nullopt;
        }

        [[nodiscard]] std::optional<OperationError>
        addEmailDeltaCalls(javelin::jmap::api::RequestBuilder& builder,
                           const std::string_view accountId, const std::string_view sinceState,
                           const std::optional<std::uint64_t> maxChanges, DeltaHandles& handles)
        {
            const auto changes = javelin::jmap::api::emailChanges({
                .accountId = std::string{accountId},
                .sinceState = std::string{sinceState},
                .maxChanges = maxChanges,
            });
            if (!changes.has_value())
                return OperationError{
                    .message = QStringLiteral("Failed to encode Email/changes."),
                };
            handles.emailChanges = builder.call(*changes, "email-changes");

            const auto created = javelin::jmap::api::emailGet(javelin::jmap::api::getRequestFrom(
                std::string{accountId}, *handles.emailChanges, "/created"));
            const auto updated = javelin::jmap::api::emailGet(javelin::jmap::api::getRequestFrom(
                std::string{accountId}, *handles.emailChanges, "/updated"));
            if (!created.has_value() || !updated.has_value())
                return OperationError{
                    .message = QStringLiteral("Failed to encode delta Email/get."),
                };
            handles.createdEmails = builder.call(*created, "created-emails");
            handles.updatedEmails = builder.call(*updated, "updated-emails");
            return std::nullopt;
        }

        [[nodiscard]] std::variant<ParsedDelta, MailDeltaRefreshSummary, OperationError>
        parseDelta(const javelin::jmap::api::ResponseEnvelope& envelope,
                   const DeltaHandles& handles)
        {
            const javelin::jmap::api::ResponseReader reader{envelope};
            ParsedDelta parsed;
            if (handles.mailboxChanges.has_value())
            {
                const auto changesResult = reader.require(*handles.mailboxChanges);
                if (const auto* error =
                        std::get_if<javelin::jmap::api::ResponseReaderError>(&changesResult))
                {
                    if (isRecoverableChangesError(*error))
                    {
                        MailDeltaRefreshSummary summary;
                        summary.mailboxNeedsFullRefresh = true;
                        summary.emailNeedsFullRefresh = handles.emailChanges.has_value();
                        return summary;
                    }
                    return operationError(*error);
                }
                parsed.mailboxChanges = std::get<MailboxChangesResponse>(changesResult);
                MailboxGetResponse created;
                MailboxGetResponse updated;
                if (const auto error = readRequired(reader, *handles.createdMailboxes, created))
                    return *error;
                if (const auto error = readRequired(reader, *handles.updatedMailboxes, updated))
                    return *error;
                const auto createdAssessment = assessMaterialization(
                    parsed.mailboxChanges->created, created.list, created.notFound,
                    parsed.mailboxChanges->newState, created.state);
                const auto updatedAssessment = assessMaterialization(
                    parsed.mailboxChanges->updated, updated.list, updated.notFound,
                    parsed.mailboxChanges->newState, updated.state);
                if (!createdAssessment.complete || !updatedAssessment.complete ||
                    created.accountId != parsed.mailboxChanges->accountId ||
                    updated.accountId != parsed.mailboxChanges->accountId)
                {
                    MailDeltaRefreshSummary summary;
                    summary.mailboxNeedsFullRefresh = true;
                    summary.emailNeedsFullRefresh = handles.emailChanges.has_value();
                    return summary;
                }
                parsed.mailboxNeedsContinuation =
                    createdAssessment.needsContinuation || updatedAssessment.needsContinuation;
                parsed.lateDestroyedMailboxes = createdAssessment.lateDestroyed;
                appendUnique(parsed.lateDestroyedMailboxes, updatedAssessment.lateDestroyed);
                parsed.mailboxes = std::move(created.list);
                parsed.mailboxes.insert(parsed.mailboxes.end(),
                                        std::make_move_iterator(updated.list.begin()),
                                        std::make_move_iterator(updated.list.end()));
            }
            if (handles.emailChanges.has_value())
            {
                const auto changesResult = reader.require(*handles.emailChanges);
                if (const auto* error =
                        std::get_if<javelin::jmap::api::ResponseReaderError>(&changesResult))
                {
                    if (isRecoverableChangesError(*error))
                    {
                        MailDeltaRefreshSummary summary;
                        summary.mailboxNeedsFullRefresh = handles.mailboxChanges.has_value();
                        summary.emailNeedsFullRefresh = true;
                        return summary;
                    }
                    return operationError(*error);
                }
                parsed.emailChanges = std::get<EmailChangesResponse>(changesResult);
                EmailGetResponse created;
                EmailGetResponse updated;
                if (const auto error = readRequired(reader, *handles.createdEmails, created))
                    return *error;
                if (const auto error = readRequired(reader, *handles.updatedEmails, updated))
                    return *error;
                const auto createdAssessment = assessMaterialization(
                    parsed.emailChanges->created, created.list, created.notFound,
                    parsed.emailChanges->newState, created.state);
                const auto updatedAssessment = assessMaterialization(
                    parsed.emailChanges->updated, updated.list, updated.notFound,
                    parsed.emailChanges->newState, updated.state);
                if (!createdAssessment.complete || !updatedAssessment.complete ||
                    created.accountId != parsed.emailChanges->accountId ||
                    updated.accountId != parsed.emailChanges->accountId)
                {
                    MailDeltaRefreshSummary summary;
                    summary.mailboxNeedsFullRefresh = handles.mailboxChanges.has_value();
                    summary.emailNeedsFullRefresh = true;
                    return summary;
                }
                parsed.emailNeedsContinuation =
                    createdAssessment.needsContinuation || updatedAssessment.needsContinuation;
                parsed.lateDestroyedEmails = createdAssessment.lateDestroyed;
                appendUnique(parsed.lateDestroyedEmails, updatedAssessment.lateDestroyed);
                parsed.emails = std::move(created.list);
                parsed.emails.insert(parsed.emails.end(),
                                     std::make_move_iterator(updated.list.begin()),
                                     std::make_move_iterator(updated.list.end()));
            }
            return parsed;
        }

    } // namespace

    MailDeltaRefreshExecutor::MailDeltaRefreshExecutor(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::api::MethodCaller& methodCaller,
        javelin::jmap::api::ApiRequestContext apiRequestContext)
        : m_databaseConnection(databaseConnection), m_methodCaller(methodCaller),
          m_apiRequestContext(std::move(apiRequestContext))
    {
    }

    QCoro::Task<MailDeltaRefreshResult>
    MailDeltaRefreshExecutor::refresh(std::string accountId,
                                      const MailDeltaRefreshRequest request) const
    {
        MailDeltaRefreshSummary summary;
        javelin::jmap::cache::SyncStateRepository states{m_databaseConnection};
        std::optional<std::string> mailboxState;
        std::optional<std::string> emailState;
        if (request.mailbox)
        {
            const auto result = states.find(syncKey(accountId, "Mailbox"));
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                co_return operationError(*error);
            const auto& record =
                std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(result);
            if (!record.has_value())
                summary.mailboxNeedsFullRefresh = true;
            else
                mailboxState = record->stateToken;
        }
        if (request.email)
        {
            const auto result = states.find(syncKey(accountId, "Email"));
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                co_return operationError(*error);
            const auto& record =
                std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(result);
            if (!record.has_value())
                summary.emailNeedsFullRefresh = true;
            else
                emailState = record->stateToken;
        }
        if (!mailboxState.has_value() && !emailState.has_value())
            co_return summary;

        ConsistencyDomainRepository consistency{m_databaseConnection};
        std::optional<RefreshFence> emailFence;
        if (emailState.has_value())
        {
            const auto fence =
                consistency.captureRefresh({.accountId = accountId, .dataType = "Email"});
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&fence))
                co_return operationError(*error);
            emailFence = std::get<RefreshFence>(fence);
        }

        javelin::jmap::api::RequestBuilder builder;
        builder.useCore().useMail();
        DeltaHandles handles;
        const auto maxChanges =
            m_apiRequestContext.requestLimits.has_value()
                ? std::optional{m_apiRequestContext.requestLimits->maxObjectsInGet}
                : std::nullopt;
        if (mailboxState.has_value())
        {
            if (const auto error =
                    addMailboxDeltaCalls(builder, accountId, *mailboxState, maxChanges, handles))
                co_return *error;
        }
        if (emailState.has_value())
        {
            if (const auto error =
                    addEmailDeltaCalls(builder, accountId, *emailState, maxChanges, handles))
                co_return *error;
        }

        const auto envelopeResult = co_await m_methodCaller.call(m_apiRequestContext, builder);
        if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&envelopeResult))
            co_return operationError(*error);
        if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&envelopeResult))
            co_return operationError(*error);
        if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&envelopeResult))
            co_return operationError(*error);

        auto parsedResult =
            parseDelta(std::get<javelin::jmap::api::ResponseEnvelope>(envelopeResult), handles);
        if (const auto* error = std::get_if<OperationError>(&parsedResult))
            co_return *error;
        if (auto* fallback = std::get_if<MailDeltaRefreshSummary>(&parsedResult))
        {
            fallback->mailboxNeedsFullRefresh =
                fallback->mailboxNeedsFullRefresh || summary.mailboxNeedsFullRefresh;
            fallback->emailNeedsFullRefresh =
                fallback->emailNeedsFullRefresh || summary.emailNeedsFullRefresh;
            co_return *fallback;
        }
        auto parsed = std::get<ParsedDelta>(std::move(parsedResult));
        if ((parsed.mailboxChanges.has_value() &&
             (parsed.mailboxChanges->accountId != accountId ||
              parsed.mailboxChanges->oldState != *mailboxState)) ||
            (parsed.emailChanges.has_value() && (parsed.emailChanges->accountId != accountId ||
                                                 parsed.emailChanges->oldState != *emailState)))
        {
            co_return OperationError{
                .code = OperationErrorCode::ServerFailure,
                .message = QStringLiteral("The mail delta response did not match the requested "
                                          "account and state."),
            };
        }
        if (emailFence.has_value())
        {
            const auto canCommit = consistency.isCurrent(*emailFence);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&canCommit))
                co_return operationError(*error);
            if (!std::get<bool>(canCommit))
            {
                summary.superseded = true;
                co_return summary;
            }
        }

        javelin::jmap::cache::EmailRepository emails{m_databaseConnection};
        std::unordered_set<std::string> createdIds;
        if (parsed.emailChanges.has_value())
            createdIds.insert(parsed.emailChanges->created.begin(),
                              parsed.emailChanges->created.end());
        for (const auto& email : parsed.emails)
        {
            const auto previousResult = emails.find(accountId, email.id);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&previousResult))
                co_return operationError(*error);
            const auto& previous =
                std::get<std::optional<javelin::jmap::domain::Email>>(previousResult);
            appendUnique(summary.changedMailboxIds, email.mailboxIds);
            if (previous.has_value())
                appendUnique(summary.changedMailboxIds, previous->mailboxIds);

            const bool queryChanged = createdIds.contains(email.id) || !previous.has_value() ||
                                      !sameSet(previous->mailboxIds, email.mailboxIds) ||
                                      previous->threadId != email.threadId ||
                                      previous->receivedAt != email.receivedAt;
            if (queryChanged)
            {
                appendUnique(summary.queryAffectedMailboxIds, email.mailboxIds);
                if (previous.has_value())
                    appendUnique(summary.queryAffectedMailboxIds, previous->mailboxIds);
            }
        }
        if (parsed.emailChanges.has_value())
        {
            summary.insertedEmailIds = parsed.emailChanges->created;
            std::erase_if(summary.insertedEmailIds,
                          [&parsed](const auto& id)
                          {
                              return std::ranges::find(parsed.lateDestroyedEmails, id) !=
                                     parsed.lateDestroyedEmails.end();
                          });
            auto destroyedIds = parsed.emailChanges->destroyed;
            appendUnique(destroyedIds, parsed.lateDestroyedEmails);
            for (const auto& destroyedId : destroyedIds)
            {
                const auto previousResult = emails.find(accountId, destroyedId);
                if (const auto* error =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&previousResult))
                    co_return operationError(*error);
                const auto& previous =
                    std::get<std::optional<javelin::jmap::domain::Email>>(previousResult);
                if (!previous.has_value())
                {
                    summary.emailNeedsFullRefresh = true;
                    continue;
                }
                appendUnique(summary.changedMailboxIds, previous->mailboxIds);
                appendUnique(summary.queryAffectedMailboxIds, previous->mailboxIds);
            }
        }

        auto transactionResult = MutationProjectionTransaction::begin(
            m_databaseConnection, QStringLiteral("Apply account mail delta"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            co_return operationError(*error);
        auto transaction = std::get<MutationProjectionTransaction>(std::move(transactionResult));
        if (emailFence.has_value())
        {
            const auto fenceCurrent = consistency.isCurrent(*emailFence);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&fenceCurrent))
                co_return operationError(*error);
            if (!std::get<bool>(fenceCurrent))
            {
                summary.superseded = true;
                co_return summary;
            }
        }
        if (parsed.mailboxChanges.has_value())
        {
            const auto advanced = states.advanceIfCurrent(
                transaction.cacheTransaction(), syncKey(accountId, "Mailbox"),
                parsed.mailboxChanges->oldState, parsed.mailboxChanges->newState);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&advanced))
                co_return operationError(*error);
            if (!std::get<bool>(advanced))
            {
                summary.superseded = true;
                co_return summary;
            }
        }
        if (parsed.emailChanges.has_value())
        {
            const auto advanced = states.advanceIfCurrent(
                transaction.cacheTransaction(), syncKey(accountId, "Email"),
                parsed.emailChanges->oldState, parsed.emailChanges->newState);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&advanced))
                co_return operationError(*error);
            if (!std::get<bool>(advanced))
            {
                summary.superseded = true;
                co_return summary;
            }
        }

        javelin::jmap::cache::MailboxRepository mailboxes{m_databaseConnection};
        if (parsed.mailboxChanges.has_value())
        {
            if (const auto error = mailboxes.upsertMany(transaction.cacheTransaction(), accountId,
                                                        parsed.mailboxes))
                co_return operationError(*error);
            if (const auto error = mailboxes.removeMany(transaction.cacheTransaction(), accountId,
                                                        parsed.mailboxChanges->destroyed))
                co_return operationError(*error);
            if (const auto error = mailboxes.removeMany(transaction.cacheTransaction(), accountId,
                                                        parsed.lateDestroyedMailboxes))
                co_return operationError(*error);
            summary.mailboxChanged = !parsed.mailboxChanges->created.empty() ||
                                     !parsed.mailboxChanges->updated.empty() ||
                                     !parsed.mailboxChanges->destroyed.empty() ||
                                     !parsed.lateDestroyedMailboxes.empty();
        }
        if (parsed.emailChanges.has_value())
        {
            javelin::jmap::cache::MailboxWindowRepository mailboxWindows{m_databaseConnection};
            for (const auto& mailboxId : summary.queryAffectedMailboxIds)
            {
                if (const auto error = mailboxWindows.invalidateMailbox(
                        transaction.cacheTransaction(), accountId, mailboxId,
                        javelin::jmap::cache::QueryWindowCoverage::Stale))
                    co_return operationError(*error);
            }
            if (const auto error =
                    emails.upsertMany(transaction.cacheTransaction(), accountId, parsed.emails))
                co_return operationError(*error);
            if (const auto error = emails.removeMany(transaction.cacheTransaction(), accountId,
                                                     parsed.emailChanges->destroyed))
                co_return operationError(*error);
            if (const auto error = emails.removeMany(transaction.cacheTransaction(), accountId,
                                                     parsed.lateDestroyedEmails))
                co_return operationError(*error);
            if (!parsed.emailChanges->created.empty() || !parsed.emailChanges->updated.empty() ||
                !parsed.emailChanges->destroyed.empty() || !parsed.lateDestroyedEmails.empty())
            {
                javelin::jmap::cache::SearchWindowRepository searches{m_databaseConnection};
                if (const auto error =
                        searches.invalidateAccount(transaction.cacheTransaction(), accountId))
                    co_return operationError(*error);
                summary.emailChanged = true;
            }
            std::vector<std::string> changedIds = parsed.emailChanges->created;
            appendUnique(changedIds, parsed.emailChanges->updated);
            if (const auto error = rebaseActiveEmailProjections(transaction, m_databaseConnection,
                                                                accountId, std::move(changedIds),
                                                                parsed.emailChanges->newState))
                co_return *error;
        }
        if (const auto error = transaction.commit())
            co_return operationError(*error);

        std::ranges::sort(summary.changedMailboxIds);
        std::ranges::sort(summary.queryAffectedMailboxIds);
        const MailDeltaRefreshRequest continuation{
            .mailbox = parsed.mailboxChanges.has_value() &&
                       (parsed.mailboxChanges->hasMoreChanges || parsed.mailboxNeedsContinuation),
            .email = parsed.emailChanges.has_value() &&
                     (parsed.emailChanges->hasMoreChanges || parsed.emailNeedsContinuation),
        };
        if (continuation.mailbox || continuation.email)
        {
            const auto continued = co_await refresh(accountId, continuation);
            if (const auto* error = std::get_if<OperationError>(&continued))
                co_return *error;
            const auto& next = std::get<MailDeltaRefreshSummary>(continued);
            summary.mailboxChanged = summary.mailboxChanged || next.mailboxChanged;
            summary.emailChanged = summary.emailChanged || next.emailChanged;
            summary.mailboxNeedsFullRefresh =
                summary.mailboxNeedsFullRefresh || next.mailboxNeedsFullRefresh;
            summary.emailNeedsFullRefresh =
                summary.emailNeedsFullRefresh || next.emailNeedsFullRefresh;
            summary.superseded = summary.superseded || next.superseded;
            appendUnique(summary.changedMailboxIds, next.changedMailboxIds);
            appendUnique(summary.queryAffectedMailboxIds, next.queryAffectedMailboxIds);
            appendUnique(summary.insertedEmailIds, next.insertedEmailIds);
            std::ranges::sort(summary.changedMailboxIds);
            std::ranges::sort(summary.queryAffectedMailboxIds);
        }
        co_return summary;
    }

} // namespace javelin::jmap::sync
