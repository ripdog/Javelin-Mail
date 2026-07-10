#include "jmap/sync/MailboxStateRefreshExecutor.h"

#include "jmap/api/MailMethods.h"
#include "jmap/api/ResponseReader.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/sync/SyncPlanner.h"
#include "jmap/sync/SyncReconciler.h"

#include <algorithm>
#include <unordered_set>

namespace javelin::jmap::sync
{

    namespace
    {
        [[nodiscard]] QString transportMessage(const javelin::jmap::api::TransportError& error)
        {
            return QStringLiteral("Transport error (%1): %2")
                .arg(QString::fromUtf8(javelin::jmap::api::toString(error.code).data()),
                     QString::fromStdString(error.message));
        }

        [[nodiscard]] QString authMessage(const javelin::jmap::api::AuthError& error)
        {
            return QStringLiteral("Authentication error (%1): %2")
                .arg(QString::fromUtf8(javelin::jmap::api::toString(error.code).data()),
                     QString::fromStdString(error.message));
        }

        [[nodiscard]] QString protocolMessage(const javelin::jmap::api::ProtocolError& error)
        {
            return QStringLiteral("Protocol error (%1): %2")
                .arg(QString::fromUtf8(javelin::jmap::api::toString(error.code).data()),
                     QString::fromStdString(error.message));
        }

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
            std::variant<javelin::jmap::api::MailboxGetResponse, MailboxStateRefreshError>>
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
                co_return MailboxStateRefreshError{
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
                co_return MailboxStateRefreshError{.message = transportMessage(*error)};
            }
            if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&envelopeResult))
            {
                co_return MailboxStateRefreshError{.message = authMessage(*error)};
            }
            if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&envelopeResult))
            {
                co_return MailboxStateRefreshError{.message = protocolMessage(*error)};
            }

            const auto& envelope = std::get<javelin::jmap::api::ResponseEnvelope>(envelopeResult);
            const javelin::jmap::api::ResponseReader reader{envelope};
            const auto mailboxResult = reader.require(mailboxHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&mailboxResult))
            {
                co_return MailboxStateRefreshError{
                    .message = QStringLiteral("Failed to read Mailbox/get response: %1")
                                   .arg(QString::fromStdString(error->message)),
                };
            }

            co_return std::get<javelin::jmap::api::MailboxGetResponse>(mailboxResult);
        }

        [[nodiscard]] QCoro::Task<std::variant<IncrementalMailboxFetch, MailboxStateRefreshError>>
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
                co_return MailboxStateRefreshError{
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
                co_return MailboxStateRefreshError{
                    .message = QStringLiteral("Failed to encode the created Mailbox/get request."),
                };
            }
            const auto createdHandle = builder.call(*createdRequest, "created-mailboxes");

            const auto updatedRequest =
                javelin::jmap::api::mailboxGet(javelin::jmap::api::getRequestFrom(
                    std::string{accountId}, changesHandle, "/updated"));
            if (!updatedRequest.has_value())
            {
                co_return MailboxStateRefreshError{
                    .message = QStringLiteral("Failed to encode the updated Mailbox/get request."),
                };
            }
            const auto updatedHandle = builder.call(*updatedRequest, "updated-mailboxes");

            const auto envelopeResult = co_await methodCaller.call(apiRequestContext, builder);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::TransportError>(&envelopeResult))
            {
                co_return MailboxStateRefreshError{.message = transportMessage(*error)};
            }
            if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&envelopeResult))
            {
                co_return MailboxStateRefreshError{.message = authMessage(*error)};
            }
            if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&envelopeResult))
            {
                co_return MailboxStateRefreshError{.message = protocolMessage(*error)};
            }

            const auto& envelope = std::get<javelin::jmap::api::ResponseEnvelope>(envelopeResult);
            const javelin::jmap::api::ResponseReader reader{envelope};
            const auto changesResult = reader.require(changesHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&changesResult))
            {
                if (isRecoverableIncrementalError(*error))
                {
                    co_return MailboxStateRefreshError{};
                }

                co_return MailboxStateRefreshError{
                    .message = QStringLiteral("Failed to read Mailbox/changes response: %1")
                                   .arg(QString::fromStdString(error->message)),
                };
            }

            const auto& changes =
                std::get<javelin::jmap::api::MailboxChangesResponse>(changesResult);
            const auto createdResult = reader.require(createdHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&createdResult))
            {
                co_return MailboxStateRefreshError{
                    .message = QStringLiteral("Failed to read created Mailbox/get response: %1")
                                   .arg(QString::fromStdString(error->message)),
                };
            }

            const auto updatedResult = reader.require(updatedHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&updatedResult))
            {
                co_return MailboxStateRefreshError{
                    .message = QStringLiteral("Failed to read updated Mailbox/get response: %1")
                                   .arg(QString::fromStdString(error->message)),
                };
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
    MailboxStateRefreshExecutor::refresh(std::string accountId) const
    {
        javelin::jmap::cache::SyncStateRepository syncStateRepository{m_databaseConnection};
        const SyncPlanner syncPlanner{syncStateRepository};
        const auto key = mailboxSyncKey(accountId);
        const auto planResult = syncPlanner.plan(key);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&planResult))
        {
            co_return MailboxStateRefreshError{.message = error->message};
        }

        const auto& plan = std::get<SyncPlan>(planResult);
        if (plan.kind == SyncPlanKind::IncrementalChanges && plan.sinceState.has_value())
        {
            const auto changesResult = co_await fetchMailboxChangesAndMailboxes(
                m_methodCaller, m_apiRequestContext, accountId, *plan.sinceState);
            const auto* incrementalFetch = std::get_if<IncrementalMailboxFetch>(&changesResult);
            if (incrementalFetch != nullptr && !incrementalFetch->changes.hasMoreChanges)
            {
                javelin::jmap::cache::MailboxRepository mailboxRepository{m_databaseConnection};
                javelin::jmap::cache::EmailRepository emailRepository{m_databaseConnection};
                SyncReconciler reconciler{mailboxRepository, emailRepository, syncStateRepository};
                if (const auto error = reconciler.applyMailboxChanges(
                        key, incrementalFetch->changes, incrementalFetch->fetched))
                {
                    co_return MailboxStateRefreshError{.message = error->message};
                }

                co_return MailboxStateRefreshSummary{
                    .mailboxCount = incrementalFetch->fetched.list.size(),
                    .usedIncrementalRefresh = true,
                };
            }

            if (const auto* error = std::get_if<MailboxStateRefreshError>(&changesResult);
                error != nullptr && !error->message.isEmpty())
            {
                co_return *error;
            }
        }

        const auto fetchedResult =
            co_await fetchMailboxes(m_methodCaller, m_apiRequestContext, accountId, std::nullopt);
        if (const auto* error = std::get_if<MailboxStateRefreshError>(&fetchedResult))
        {
            co_return *error;
        }

        const auto& fetched = std::get<javelin::jmap::api::MailboxGetResponse>(fetchedResult);
        javelin::jmap::cache::MailboxRepository mailboxRepository{m_databaseConnection};
        if (const auto error = mailboxRepository.replaceAll(accountId, fetched.list))
        {
            co_return MailboxStateRefreshError{.message = error->message};
        }
        if (const auto error = syncStateRepository.upsert(key, fetched.state))
        {
            co_return MailboxStateRefreshError{.message = error->message};
        }

        co_return MailboxStateRefreshSummary{
            .mailboxCount = fetched.list.size(),
            .usedIncrementalRefresh = false,
        };
    }

} // namespace javelin::jmap::sync
