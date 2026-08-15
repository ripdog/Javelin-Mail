#include "jmap/sync/MailboxStateRefreshExecutor.h"

#include "jmap/api/MailMethods.h"
#include "jmap/api/ResponseReader.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/sync/ConsistencyDomain.h"
#include "jmap/sync/MailboxMutationJournal.h"
#include "jmap/sync/SyncPlanner.h"

#include <algorithm>
#include <unordered_set>

namespace javelin::jmap::sync
{

    namespace
    {
        [[nodiscard]] javelin::jmap::cache::SyncStateKey
        mailboxSyncKey(const std::string_view accountId)
        {
            return javelin::jmap::cache::SyncStateKey{
                .accountId = std::string{accountId},
                .objectType = "Mailbox",
                .queryKey = {},
            };
        }

        [[nodiscard]] bool
        isRecoverableIncrementalError(const javelin::jmap::api::ResponseReaderError& error)
        {
            if (error.code != javelin::jmap::api::ResponseReaderErrorCode::MethodError ||
                !error.methodError.has_value())
            {
                return false;
            }

            return error.methodError->type == "cannotCalculateChanges" ||
                   error.methodError->type == "tooManyChanges";
        }

        struct IncrementalMailboxFetch
        {
            javelin::jmap::api::MailboxChangesResponse changes;
            javelin::jmap::api::MailboxGetResponse fetched;
        };

        [[nodiscard]] javelin::jmap::api::MailboxGetResponse
        mergeMailboxFetches(const std::string_view accountId,
                            const javelin::jmap::api::MailboxChangesResponse& changes,
                            javelin::jmap::api::MailboxGetResponse created,
                            const javelin::jmap::api::MailboxGetResponse& updated)
        {
            javelin::jmap::api::MailboxGetResponse fetched{
                .accountId = std::string{accountId},
                .state = changes.newState,
                .list = std::move(created.list),
                .notFound = std::move(created.notFound),
            };

            std::unordered_set<std::string> seen;
            seen.reserve(changes.created.size() + changes.updated.size());
            for (const auto& mailbox : fetched.list)
            {
                seen.insert(mailbox.id);
            }
            for (const auto& mailbox : updated.list)
            {
                if (seen.insert(mailbox.id).second)
                {
                    fetched.list.push_back(mailbox);
                }
            }
            fetched.notFound.insert(fetched.notFound.end(), updated.notFound.begin(),
                                    updated.notFound.end());

            return fetched;
        }

        [[nodiscard]] QCoro::Task<
            std::variant<javelin::jmap::api::MailboxGetResponse, OperationError>>
        fetchMailboxes(javelin::jmap::api::MethodCaller& methodCaller,
                       javelin::jmap::api::ApiRequestContext apiRequestContext,
                       const std::string_view accountId,
                       std::optional<std::vector<std::string>> ids)
        {
            const auto mailboxRequest = javelin::jmap::api::mailboxGet({
                .accountId = std::string{accountId},
                .ids = std::move(ids),
                .idsReference = std::nullopt,
                .properties = std::nullopt,
            });
            if (!mailboxRequest.has_value())
            {
                co_return OperationError{
                    .message = QStringLiteral("Failed to encode the Mailbox/get request."),
                };
            }

            javelin::jmap::api::RequestBuilder builder;
            builder.useCore().useMail();
            const auto mailboxHandle = builder.call(*mailboxRequest, "mailboxes");

            const auto envelopeResult = co_await methodCaller.call(apiRequestContext, builder);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::TransportError>(&envelopeResult))
            {
                co_return operationError(*error);
            }
            if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&envelopeResult))
            {
                co_return operationError(*error);
            }
            if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&envelopeResult))
            {
                co_return operationError(*error);
            }

            const auto& envelope = std::get<javelin::jmap::api::ResponseEnvelope>(envelopeResult);
            const javelin::jmap::api::ResponseReader reader{envelope};
            const auto mailboxResult = reader.require(mailboxHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&mailboxResult))
            {
                co_return operationError(*error);
            }

            co_return std::get<javelin::jmap::api::MailboxGetResponse>(mailboxResult);
        }

        [[nodiscard]] QCoro::Task<std::variant<IncrementalMailboxFetch, OperationError>>
        fetchMailboxChangesAndMailboxes(javelin::jmap::api::MethodCaller& methodCaller,
                                        javelin::jmap::api::ApiRequestContext apiRequestContext,
                                        const std::string_view accountId,
                                        const std::string_view sinceState)
        {
            const auto changesRequest = javelin::jmap::api::mailboxChanges({
                .accountId = std::string{accountId},
                .sinceState = std::string{sinceState},
                .maxChanges = std::nullopt,
            });
            if (!changesRequest.has_value())
            {
                co_return OperationError{
                    .message = QStringLiteral("Failed to encode the Mailbox/changes request."),
                };
            }

            javelin::jmap::api::RequestBuilder builder;
            builder.useCore().useMail();
            const auto changesHandle = builder.call(*changesRequest, "mailbox-changes");
            const auto createdRequest =
                javelin::jmap::api::mailboxGet(javelin::jmap::api::getRequestFrom(
                    std::string{accountId}, changesHandle, "/created"));
            if (!createdRequest.has_value())
            {
                co_return OperationError{
                    .message = QStringLiteral("Failed to encode the created Mailbox/get request."),
                };
            }
            const auto createdHandle = builder.call(*createdRequest, "created-mailboxes");

            const auto updatedRequest =
                javelin::jmap::api::mailboxGet(javelin::jmap::api::getRequestFrom(
                    std::string{accountId}, changesHandle, "/updated"));
            if (!updatedRequest.has_value())
            {
                co_return OperationError{
                    .message = QStringLiteral("Failed to encode the updated Mailbox/get request."),
                };
            }
            const auto updatedHandle = builder.call(*updatedRequest, "updated-mailboxes");

            const auto envelopeResult = co_await methodCaller.call(apiRequestContext, builder);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::TransportError>(&envelopeResult))
            {
                co_return operationError(*error);
            }
            if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&envelopeResult))
            {
                co_return operationError(*error);
            }
            if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&envelopeResult))
            {
                co_return operationError(*error);
            }

            const auto& envelope = std::get<javelin::jmap::api::ResponseEnvelope>(envelopeResult);
            const javelin::jmap::api::ResponseReader reader{envelope};
            const auto changesResult = reader.require(changesHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&changesResult))
            {
                if (isRecoverableIncrementalError(*error))
                {
                    co_return OperationError{};
                }

                co_return operationError(*error);
            }

            const auto& changes =
                std::get<javelin::jmap::api::MailboxChangesResponse>(changesResult);
            const auto createdResult = reader.require(createdHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&createdResult))
            {
                co_return operationError(*error);
            }

            const auto updatedResult = reader.require(updatedHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&updatedResult))
            {
                co_return operationError(*error);
            }

            co_return IncrementalMailboxFetch{
                .changes = changes,
                .fetched = mergeMailboxFetches(
                    accountId, changes,
                    std::get<javelin::jmap::api::MailboxGetResponse>(createdResult),
                    std::get<javelin::jmap::api::MailboxGetResponse>(updatedResult)),
            };
        }

    } // namespace

    MailboxStateRefreshExecutor::MailboxStateRefreshExecutor(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::api::MethodCaller& methodCaller,
        javelin::jmap::api::ApiRequestContext apiRequestContext)
        : m_databaseConnection(databaseConnection), m_methodCaller(methodCaller),
          m_apiRequestContext(std::move(apiRequestContext))
    {
    }

    QCoro::Task<MailboxStateRefreshResult>
    MailboxStateRefreshExecutor::refresh(std::string accountId, std::string remoteAccountId) const
    {
        if (remoteAccountId.empty())
            remoteAccountId = accountId;

        javelin::jmap::cache::SyncStateRepository syncStateRepository{m_databaseConnection};
        const SyncPlanner syncPlanner{syncStateRepository};
        const auto key = mailboxSyncKey(accountId);
        const auto planResult = syncPlanner.plan(key);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&planResult))
        {
            co_return javelin::jmap::operationError(*error);
        }

        const auto& plan = std::get<SyncPlan>(planResult);
        ConsistencyDomainRepository consistency{m_databaseConnection};
        const auto fenceResult =
            consistency.captureRefresh({.accountId = accountId, .dataType = "Mailbox"});
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&fenceResult))
            co_return javelin::jmap::operationError(*error);
        const auto fence = std::get<RefreshFence>(fenceResult);

        if (plan.kind == SyncPlanKind::IncrementalChanges && plan.sinceState.has_value())
        {
            const auto changesResult = co_await fetchMailboxChangesAndMailboxes(
                m_methodCaller, m_apiRequestContext, remoteAccountId, *plan.sinceState);
            const auto* incrementalFetch = std::get_if<IncrementalMailboxFetch>(&changesResult);
            if (incrementalFetch != nullptr && !incrementalFetch->changes.hasMoreChanges)
            {
                auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
                    m_databaseConnection, QStringLiteral("Apply mailbox state delta"));
                if (const auto* error =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
                    co_return javelin::jmap::operationError(*error);
                auto transaction = std::get<javelin::jmap::cache::DatabaseTransaction>(
                    std::move(transactionResult));
                const auto current = consistency.isCurrent(fence);
                if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&current))
                    co_return javelin::jmap::operationError(*error);
                const auto advanced = syncStateRepository.advanceIfCurrent(
                    transaction, key, incrementalFetch->changes.oldState,
                    incrementalFetch->changes.newState);
                if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&advanced))
                    co_return javelin::jmap::operationError(*error);
                if (!std::get<bool>(current) || !std::get<bool>(advanced))
                    co_return MailboxStateRefreshSummary{.superseded = true};

                javelin::jmap::cache::MailboxRepository mailboxRepository{m_databaseConnection};
                if (const auto error = mailboxRepository.upsertMany(transaction, accountId,
                                                                    incrementalFetch->fetched.list))
                    co_return javelin::jmap::operationError(*error);
                if (const auto error = mailboxRepository.removeMany(
                        transaction, accountId, incrementalFetch->changes.destroyed))
                    co_return javelin::jmap::operationError(*error);
                MailboxMutationJournal mutations{m_databaseConnection, mailboxRepository};
                if (const auto error = mutations.rebase(transaction, accountId))
                    co_return javelin::jmap::operationError(*error);
                if (const auto error = transaction.commit())
                    co_return javelin::jmap::operationError(*error);

                co_return MailboxStateRefreshSummary{
                    .mailboxCount = incrementalFetch->fetched.list.size(),
                    .usedIncrementalRefresh = true,
                    .changed = !incrementalFetch->changes.created.empty() ||
                               !incrementalFetch->changes.updated.empty() ||
                               !incrementalFetch->changes.destroyed.empty(),
                };
            }

            if (const auto* error = std::get_if<OperationError>(&changesResult);
                error != nullptr && !error->message.isEmpty())
            {
                co_return *error;
            }
        }

        const auto fetchedResult = co_await fetchMailboxes(m_methodCaller, m_apiRequestContext,
                                                           remoteAccountId, std::nullopt);
        if (const auto* error = std::get_if<OperationError>(&fetchedResult))
        {
            co_return *error;
        }

        const auto& fetched = std::get<javelin::jmap::api::MailboxGetResponse>(fetchedResult);
        auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
            m_databaseConnection, QStringLiteral("Apply mailbox state refresh"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            co_return javelin::jmap::operationError(*error);
        auto transaction =
            std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));
        const auto current = consistency.isCurrent(fence);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&current))
            co_return javelin::jmap::operationError(*error);
        const auto expected = plan.sinceState.has_value()
                                  ? std::optional<std::string_view>{*plan.sinceState}
                                  : std::nullopt;
        const auto advanced =
            syncStateRepository.replaceIfCurrent(transaction, key, expected, fetched.state);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&advanced))
            co_return javelin::jmap::operationError(*error);
        if (!std::get<bool>(current) || !std::get<bool>(advanced))
            co_return MailboxStateRefreshSummary{.superseded = true};

        javelin::jmap::cache::MailboxRepository mailboxRepository{m_databaseConnection};
        if (const auto error = mailboxRepository.replaceAll(transaction, accountId, fetched.list))
            co_return javelin::jmap::operationError(*error);
        MailboxMutationJournal mutations{m_databaseConnection, mailboxRepository};
        if (const auto error = mutations.rebase(transaction, accountId))
            co_return javelin::jmap::operationError(*error);
        if (const auto error = transaction.commit())
            co_return javelin::jmap::operationError(*error);

        co_return MailboxStateRefreshSummary{
            .mailboxCount = fetched.list.size(),
            .usedIncrementalRefresh = false,
            .changed = true,
        };
    }

} // namespace javelin::jmap::sync
