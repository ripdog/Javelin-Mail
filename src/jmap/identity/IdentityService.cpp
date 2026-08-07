#include "jmap/identity/IdentityService.h"

#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MailMethods.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/RequestBuilder.h"
#include "jmap/api/ResponseReader.h"
#include "jmap/api/Session.h"
#include "jmap/auth/Auth.h"
#include "jmap/cache/IdentityRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/identity/IdentityMutationJournal.h"

#include <QUuid>

#include <algorithm>
#include <array>
#include <unordered_set>
#include <utility>

namespace javelin::jmap::identity
{
    namespace
    {
        constexpr std::size_t maximumSignatureBytes = 128U * 1024U;
        constexpr std::size_t maximumIdentityBytes = 256U * 1024U;

        struct ResolvedContext
        {
            std::string ownerAccountId;
            std::string accountId;
            LiveConnectionSettings settings;
            api::Session session;
        };

        struct SetAndGetResponse
        {
            api::IdentitySetResponse set;
            std::optional<api::IdentityGetResponse> get;
        };

        struct IncrementalIdentityChanges
        {
            std::vector<javelin::jmap::domain::Identity> upserts;
            std::vector<std::string> destroyedIds;
            std::string newState;
        };

        [[nodiscard]] OperationError error(const OperationErrorCode code, QString message,
                                           std::optional<std::string> type = std::nullopt)
        {
            return {.code = code, .message = std::move(message), .protocolType = std::move(type)};
        }

        [[nodiscard]] javelin::jmap::auth::AccountCredentials
        credentials(const ResolvedContext& context)
        {
            return {
                .accountId = context.ownerAccountId,
                .emailAddress = context.settings.loginEmail,
                .sessionUrl = context.settings.sessionUrl,
                .token =
                    {
                        .accessToken = context.settings.apiKey,
                        .refreshToken = std::nullopt,
                        .expiry = std::nullopt,
                    },
            };
        }

        [[nodiscard]] api::ApiRequestContext requestContext(const ResolvedContext& context)
        {
            return {
                .credentials = credentials(context),
                .apiUrl = context.session.apiUrl,
                .requestLimits = api::coreRequestLimits(context.session),
            };
        }

        [[nodiscard]] std::variant<ResolvedContext, OperationError>
        resolveContext(cache::DatabaseConnection& connection, LiveConnectionSettings settings,
                       std::string ownerAccountId, std::string accountId)
        {
            if (settings.sessionUrl.empty() || settings.loginEmail.empty() ||
                settings.apiKey.empty())
            {
                return error(OperationErrorCode::PreconditionFailed,
                             QStringLiteral("This account is not ready for JMAP requests."));
            }
            cache::SessionRepository sessions{connection};
            const auto loaded = sessions.load(ownerAccountId);
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&loaded))
                return operationError(*cacheError);
            const auto& session = std::get<std::optional<api::Session>>(loaded);
            if (!session.has_value())
            {
                return error(
                    OperationErrorCode::PreconditionFailed,
                    QStringLiteral("No cached JMAP session is available for this account."));
            }
            if (!session->capabilities.submission)
            {
                return error(
                    OperationErrorCode::UnsupportedCapability,
                    QStringLiteral("This server does not support JMAP submission identities."));
            }
            const auto account = session->accounts.find(accountId);
            if (account == session->accounts.end())
            {
                return error(OperationErrorCode::NotFound,
                             QStringLiteral("The sender Identity account is no longer available."));
            }
            if (!account->second.accountCapabilities.submission)
            {
                return error(
                    OperationErrorCode::UnsupportedCapability,
                    QStringLiteral("This account does not support JMAP submission identities."));
            }
            return ResolvedContext{
                .ownerAccountId = std::move(ownerAccountId),
                .accountId = std::move(accountId),
                .settings = std::move(settings),
                .session = *session,
            };
        }

        [[nodiscard]] bool sameAddress(const javelin::jmap::domain::EmailAddress& left,
                                       const javelin::jmap::domain::EmailAddress& right)
        {
            return left.name == right.name && left.email == right.email;
        }

        [[nodiscard]] bool
        sameAddresses(const std::vector<javelin::jmap::domain::EmailAddress>& left,
                      const std::vector<javelin::jmap::domain::EmailAddress>& right)
        {
            return left.size() == right.size() && std::ranges::equal(left, right, sameAddress);
        }

        [[nodiscard]] bool sameWritableIdentityFields(const javelin::jmap::domain::Identity& left,
                                                      const javelin::jmap::domain::Identity& right)
        {
            return left.name == right.name && left.email == right.email &&
                   sameAddresses(left.replyTo, right.replyTo) &&
                   sameAddresses(left.bcc, right.bcc) &&
                   left.textSignature.value_or(std::string{}) ==
                       right.textSignature.value_or(std::string{}) &&
                   left.htmlSignature.value_or(std::string{}) ==
                       right.htmlSignature.value_or(std::string{});
        }

        [[nodiscard]] bool sameWritableIdentity(const javelin::jmap::domain::Identity& left,
                                                const javelin::jmap::domain::Identity& right)
        {
            return left.id == right.id && sameWritableIdentityFields(left, right);
        }

        [[nodiscard]] std::optional<OperationError>
        validateIdentity(const javelin::jmap::domain::Identity& identity, const bool creating)
        {
            if (identity.email.empty())
                return error(OperationErrorCode::InvalidUserInput,
                             QStringLiteral("A sender email address is required."));
            if (!creating && identity.id.empty())
                return error(OperationErrorCode::InvalidRequest,
                             QStringLiteral("The Identity id is required for an update."));
            const auto textBytes = identity.textSignature.value_or(std::string{}).size();
            const auto htmlBytes = identity.htmlSignature.value_or(std::string{}).size();
            if (textBytes > maximumSignatureBytes || htmlBytes > maximumSignatureBytes ||
                textBytes + htmlBytes > maximumIdentityBytes)
            {
                return error(OperationErrorCode::InvalidUserInput,
                             QStringLiteral("The signature is too large."));
            }
            return std::nullopt;
        }

        [[nodiscard]] OperationError setError(const api::IdentitySetError& rejected,
                                              const QString& fallback)
        {
            OperationErrorCode code = OperationErrorCode::ServerFailure;
            if (rejected.type == "forbidden" || rejected.type == "forbiddenFrom")
                code = OperationErrorCode::PermissionDenied;
            else if (rejected.type == "invalidArguments" || rejected.type == "invalidProperties")
                code = OperationErrorCode::InvalidUserInput;
            else if (rejected.type == "notFound")
                code = OperationErrorCode::NotFound;
            else if (rejected.type == "stateMismatch")
                code = OperationErrorCode::Conflict;
            return error(code,
                         rejected.description.has_value()
                             ? QString::fromStdString(*rejected.description)
                             : fallback,
                         rejected.type);
        }

        [[nodiscard]] std::variant<IdentitySnapshot, OperationError>
        cachedSnapshot(cache::IdentityRepository& repository, const std::string_view accountId)
        {
            auto identities = repository.listByAccount(accountId);
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&identities))
                return operationError(*cacheError);
            auto pending = repository.listPendingCreates(accountId);
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&pending))
                return operationError(*cacheError);
            return IdentitySnapshot{
                .identities =
                    std::get<std::vector<javelin::jmap::domain::Identity>>(std::move(identities)),
                .pendingCreates =
                    std::get<std::vector<cache::PendingIdentityCreate>>(std::move(pending)),
            };
        }

        [[nodiscard]] QCoro::Task<std::variant<api::IdentityGetResponse, OperationError>>
        fetchAll(api::JmapMethodTransport& transport, const ResolvedContext& context)
        {
            api::RequestBuilder builder;
            builder.useCore().useCapability(std::string{api::submissionCapabilityUri});
            const auto request = api::identityGet({
                .accountId = context.accountId,
                .ids = std::nullopt,
                .idsReference = std::nullopt,
                .properties = std::nullopt,
            });
            if (!request.has_value())
            {
                co_return error(OperationErrorCode::ProtocolViolation,
                                QStringLiteral("Failed to encode Identity/get."));
            }
            const auto handle = builder.call(*request, "identity-refresh");
            api::MethodCaller caller{transport};
            auto result = co_await caller.call(requestContext(context), builder);
            if (const auto* transportError = std::get_if<api::TransportError>(&result))
                co_return operationError(*transportError);
            if (const auto* authError = std::get_if<api::AuthError>(&result))
                co_return operationError(*authError);
            if (const auto* protocolError = std::get_if<api::ProtocolError>(&result))
                co_return operationError(*protocolError);
            const api::ResponseReader reader{std::get<api::ResponseEnvelope>(result)};
            auto response = reader.require(handle);
            if (const auto* readerError = std::get_if<api::ResponseReaderError>(&response))
                co_return operationError(*readerError);
            auto value = std::get<api::IdentityGetResponse>(std::move(response));
            if (value.accountId != context.accountId || !value.notFound.empty())
            {
                co_return error(
                    OperationErrorCode::ProtocolViolation,
                    QStringLiteral("The server returned an invalid Identity snapshot."));
            }
            co_return value;
        }

        [[nodiscard]] QCoro::Task<std::variant<api::IdentityChangesResponse, OperationError>>
        fetchChangesPage(api::JmapMethodTransport& transport, const ResolvedContext& context,
                         const std::string& sinceState)
        {
            api::RequestBuilder builder;
            builder.useCore().useCapability(std::string{api::submissionCapabilityUri});
            const auto request = api::identityChanges({
                .accountId = context.accountId,
                .sinceState = sinceState,
                .maxChanges = 256,
            });
            if (!request.has_value())
            {
                co_return error(OperationErrorCode::ProtocolViolation,
                                QStringLiteral("Failed to encode Identity/changes."));
            }
            const auto handle = builder.call(*request, "identity-changes");
            api::MethodCaller caller{transport};
            auto result = co_await caller.call(requestContext(context), builder);
            if (const auto* transportError = std::get_if<api::TransportError>(&result))
                co_return operationError(*transportError);
            if (const auto* authError = std::get_if<api::AuthError>(&result))
                co_return operationError(*authError);
            if (const auto* protocolError = std::get_if<api::ProtocolError>(&result))
                co_return operationError(*protocolError);
            const api::ResponseReader reader{std::get<api::ResponseEnvelope>(result)};
            auto response = reader.require(handle);
            if (const auto* readerError = std::get_if<api::ResponseReaderError>(&response))
                co_return operationError(*readerError);
            auto value = std::get<api::IdentityChangesResponse>(std::move(response));
            if (value.accountId != context.accountId || value.oldState != sinceState ||
                value.newState.empty() || (value.hasMoreChanges && value.newState == sinceState))
            {
                co_return error(
                    OperationErrorCode::ProtocolViolation,
                    QStringLiteral("The server returned an incoherent Identity change page."));
            }
            co_return value;
        }

        [[nodiscard]] QCoro::Task<
            std::variant<std::vector<javelin::jmap::domain::Identity>, OperationError>>
        fetchIdentities(api::JmapMethodTransport& transport, const ResolvedContext& context,
                        const std::vector<std::string>& identityIds)
        {
            if (identityIds.empty())
                co_return std::vector<javelin::jmap::domain::Identity>{};
            api::RequestBuilder builder;
            builder.useCore().useCapability(std::string{api::submissionCapabilityUri});
            const auto request = api::identityGet({
                .accountId = context.accountId,
                .ids = identityIds,
                .idsReference = std::nullopt,
                .properties = std::nullopt,
            });
            if (!request.has_value())
            {
                co_return error(OperationErrorCode::ProtocolViolation,
                                QStringLiteral("Failed to encode incremental Identity/get."));
            }
            const auto handle = builder.call(*request, "identity-incremental-get");
            api::MethodCaller caller{transport};
            auto result = co_await caller.call(requestContext(context), builder);
            if (const auto* transportError = std::get_if<api::TransportError>(&result))
                co_return operationError(*transportError);
            if (const auto* authError = std::get_if<api::AuthError>(&result))
                co_return operationError(*authError);
            if (const auto* protocolError = std::get_if<api::ProtocolError>(&result))
                co_return operationError(*protocolError);
            const api::ResponseReader reader{std::get<api::ResponseEnvelope>(result)};
            auto response = reader.require(handle);
            if (const auto* readerError = std::get_if<api::ResponseReaderError>(&response))
                co_return operationError(*readerError);
            auto value = std::get<api::IdentityGetResponse>(std::move(response));
            if (value.accountId != context.accountId || !value.notFound.empty() ||
                value.list.size() != identityIds.size())
            {
                co_return error(OperationErrorCode::ProtocolViolation,
                                QStringLiteral("The incremental Identity snapshot is incomplete."));
            }
            std::unordered_set<std::string> returnedIds;
            for (const auto& identity : value.list)
            {
                if (!returnedIds.insert(identity.id).second ||
                    std::ranges::find(identityIds, identity.id) == identityIds.end())
                {
                    co_return error(
                        OperationErrorCode::ProtocolViolation,
                        QStringLiteral("The incremental Identity snapshot is invalid."));
                }
            }
            co_return std::move(value.list);
        }

        [[nodiscard]] QCoro::Task<std::variant<IncrementalIdentityChanges, OperationError>>
        fetchIncremental(api::JmapMethodTransport& transport, const ResolvedContext& context,
                         std::string sinceState)
        {
            std::vector<std::string> changedIds;
            std::vector<std::string> destroyedIds;
            std::unordered_set<std::string> changedSet;
            std::unordered_set<std::string> destroyedSet;
            constexpr std::size_t maximumPages = 64;
            for (std::size_t page = 0; page < maximumPages; ++page)
            {
                auto pageResult = co_await fetchChangesPage(transport, context, sinceState);
                if (const auto* pageError = std::get_if<OperationError>(&pageResult))
                    co_return *pageError;
                auto changes = std::get<api::IdentityChangesResponse>(std::move(pageResult));
                const auto noteChanged = [&](const std::string& identityId)
                {
                    if (destroyedSet.contains(identityId))
                        return;
                    if (changedSet.insert(identityId).second)
                        changedIds.push_back(identityId);
                };
                for (const auto& identityId : changes.created)
                    noteChanged(identityId);
                for (const auto& identityId : changes.updated)
                    noteChanged(identityId);
                for (const auto& identityId : changes.destroyed)
                {
                    changedSet.erase(identityId);
                    std::erase(changedIds, identityId);
                    if (destroyedSet.insert(identityId).second)
                        destroyedIds.push_back(identityId);
                }
                sinceState = std::move(changes.newState);
                if (!changes.hasMoreChanges)
                {
                    auto fetched = co_await fetchIdentities(transport, context, changedIds);
                    if (const auto* fetchError = std::get_if<OperationError>(&fetched))
                        co_return *fetchError;
                    co_return IncrementalIdentityChanges{
                        .upserts = std::get<std::vector<javelin::jmap::domain::Identity>>(
                            std::move(fetched)),
                        .destroyedIds = std::move(destroyedIds),
                        .newState = std::move(sinceState),
                    };
                }
            }
            co_return error(OperationErrorCode::ProtocolViolation,
                            QStringLiteral("Identity/changes exceeded the page limit."));
        }

        [[nodiscard]] QCoro::Task<std::variant<SetAndGetResponse, OperationError>>
        setAndFetch(api::JmapMethodTransport& transport, const ResolvedContext& context,
                    const api::IdentitySetRequest& setRequest, bool& dispatched)
        {
            api::RequestBuilder builder;
            builder.useCore().useCapability(std::string{api::submissionCapabilityUri});
            const auto encodedSet = api::identitySet(setRequest);
            const auto encodedGet = api::identityGet({
                .accountId = context.accountId,
                .ids = std::nullopt,
                .idsReference = std::nullopt,
                .properties = std::nullopt,
            });
            if (!encodedSet.has_value() || !encodedGet.has_value())
            {
                co_return error(OperationErrorCode::ProtocolViolation,
                                QStringLiteral("Failed to encode the Identity mutation."));
            }
            const auto setHandle = builder.call(*encodedSet, "identity-set");
            const auto getHandle = builder.call(*encodedGet, "identity-after-set");
            api::MethodCaller caller{transport};
            auto result = co_await caller.call(requestContext(context), builder, {},
                                               [&dispatched] { dispatched = true; });
            if (const auto* transportError = std::get_if<api::TransportError>(&result))
                co_return operationError(*transportError);
            if (const auto* authError = std::get_if<api::AuthError>(&result))
                co_return operationError(*authError);
            if (const auto* protocolError = std::get_if<api::ProtocolError>(&result))
                co_return operationError(*protocolError);
            const api::ResponseReader reader{std::get<api::ResponseEnvelope>(result)};
            auto set = reader.require(setHandle);
            if (const auto* readerError = std::get_if<api::ResponseReaderError>(&set))
                co_return operationError(*readerError);
            auto get = reader.get(getHandle);
            std::optional<api::IdentityGetResponse> snapshot;
            if (auto* value = std::get_if<api::IdentityGetResponse>(&get);
                value != nullptr && value->accountId == context.accountId &&
                value->notFound.empty())
                snapshot = std::move(*value);
            co_return SetAndGetResponse{
                .set = std::get<api::IdentitySetResponse>(std::move(set)),
                .get = std::move(snapshot),
            };
        }

        [[nodiscard]] std::vector<javelin::jmap::domain::Identity>
        fallbackAcceptedIdentities(cache::IdentityRepository& repository,
                                   const IdentityMutationRecord& mutation,
                                   const api::IdentitySetResponse& response)
        {
            auto listed = repository.listByAccount(mutation.accountId);
            if (const auto* identities =
                    std::get_if<std::vector<javelin::jmap::domain::Identity>>(&listed))
            {
                auto result = *identities;
                if (mutation.kind == IdentityMutationKind::Create &&
                    mutation.creationId.has_value())
                {
                    const auto created = response.created.find(*mutation.creationId);
                    if (created != response.created.end())
                    {
                        auto identity = *mutation.after;
                        identity.id = created->second;
                        identity.mayDelete = false;
                        result.push_back(std::move(identity));
                    }
                }
                return result;
            }
            return {};
        }

        [[nodiscard]] std::optional<javelin::jmap::domain::Identity>
        findIdentity(const std::vector<javelin::jmap::domain::Identity>& identities,
                     const std::string_view id)
        {
            const auto found =
                std::ranges::find(identities, id, &javelin::jmap::domain::Identity::id);
            return found == identities.end()
                       ? std::nullopt
                       : std::optional<javelin::jmap::domain::Identity>{*found};
        }
    } // namespace

    IdentityService::IdentityService(cache::DatabaseConnection& connection,
                                     api::JmapMethodTransport& methodTransport)
        : m_connection(connection), m_methodTransport(methodTransport)
    {
    }

    QCoro::Task<IdentityListResult> IdentityService::refresh(LiveConnectionSettings settings,
                                                             std::string accountId) const
    {
        auto ownerAccountId = accountId;
        co_return co_await refresh(std::move(settings), std::move(ownerAccountId),
                                   std::move(accountId));
    }

    QCoro::Task<IdentityListResult> IdentityService::refresh(LiveConnectionSettings settings,
                                                             std::string ownerAccountId,
                                                             std::string accountId) const
    {
        auto resolved = resolveContext(m_connection, std::move(settings), std::move(ownerAccountId),
                                       std::move(accountId));
        if (const auto* contextError = std::get_if<OperationError>(&resolved))
            co_return *contextError;
        const auto& context = std::get<ResolvedContext>(resolved);

        cache::IdentityRepository repository{m_connection};
        IdentityMutationJournal journal{m_connection, repository};
        auto activeResult = journal.listActive(context.accountId);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&activeResult))
            co_return operationError(*cacheError);
        const auto& active = std::get<std::vector<IdentityMutationRecord>>(activeResult);

        if (active.empty())
        {
            const auto stateResult = repository.state(context.accountId);
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&stateResult))
                co_return operationError(*cacheError);
            const auto& cachedState = std::get<std::optional<std::string>>(stateResult);
            if (cachedState.has_value() && !cachedState->empty())
            {
                auto incremental =
                    co_await fetchIncremental(m_methodTransport, context, *cachedState);
                if (auto* changes = std::get_if<IncrementalIdentityChanges>(&incremental))
                {
                    if (const auto cacheError =
                            repository.applyChanges(context.accountId, changes->upserts,
                                                    changes->destroyedIds, changes->newState))
                        co_return operationError(*cacheError);
                    auto snapshot = cachedSnapshot(repository, context.accountId);
                    if (const auto* cacheError = std::get_if<OperationError>(&snapshot))
                        co_return *cacheError;
                    co_return std::get<IdentitySnapshot>(std::move(snapshot));
                }
                const auto& incrementalError = std::get<OperationError>(incremental);
                if (isTransientError(incrementalError) || isAuthenticationError(incrementalError))
                    co_return incrementalError;
            }
        }

        auto fetched = co_await fetchAll(m_methodTransport, context);
        if (const auto* fetchError = std::get_if<OperationError>(&fetched))
            co_return *fetchError;
        auto server = std::get<api::IdentityGetResponse>(std::move(fetched));
        if (active.empty())
        {
            if (const auto cacheError =
                    repository.replaceAll(context.accountId, server.list, server.state))
                co_return operationError(*cacheError);
        }
        else if (active.size() == 1 && active.front().status == sync::MutationStatus::Unknown)
        {
            const auto& mutation = active.front();
            bool accepted = false;
            if (mutation.kind == IdentityMutationKind::Update)
            {
                const auto serverIdentity = findIdentity(server.list, mutation.objectId);
                accepted = serverIdentity.has_value() && mutation.after.has_value() &&
                           sameWritableIdentity(*serverIdentity, *mutation.after);
            }
            else if (mutation.kind == IdentityMutationKind::Create && mutation.after.has_value())
            {
                const auto cachedResult = repository.listByAccount(context.accountId);
                if (const auto* cacheError = std::get_if<cache::DatabaseError>(&cachedResult))
                    co_return operationError(*cacheError);
                const auto& cached =
                    std::get<std::vector<javelin::jmap::domain::Identity>>(cachedResult);
                std::size_t matchingNewIdentities = 0;
                for (const auto& serverIdentity : server.list)
                {
                    if (findIdentity(cached, serverIdentity.id).has_value())
                        continue;
                    if (sameWritableIdentityFields(serverIdentity, *mutation.after))
                        ++matchingNewIdentities;
                }
                accepted = matchingNewIdentities == 1;
            }
            else if (mutation.kind == IdentityMutationKind::Destroy)
                accepted = !findIdentity(server.list, mutation.objectId).has_value();
            if (accepted)
            {
                if (const auto cacheError = journal.accept(mutation, server.list, server.state))
                    co_return operationError(*cacheError);
            }
        }

        auto snapshot = cachedSnapshot(repository, context.accountId);
        if (const auto* cacheError = std::get_if<OperationError>(&snapshot))
            co_return *cacheError;
        co_return std::get<IdentitySnapshot>(std::move(snapshot));
    }

    QCoro::Task<IdentitySaveResult>
    IdentityService::save(LiveConnectionSettings settings, std::string accountId,
                          javelin::jmap::domain::Identity identity,
                          std::optional<std::string> operationGroupId,
                          std::function<void()> projectionCommitted) const
    {
        auto ownerAccountId = accountId;
        co_return co_await save(std::move(settings), std::move(ownerAccountId),
                                std::move(accountId), std::move(identity),
                                std::move(operationGroupId), std::move(projectionCommitted));
    }

    QCoro::Task<IdentitySaveResult>
    IdentityService::save(LiveConnectionSettings settings, std::string ownerAccountId,
                          std::string accountId, javelin::jmap::domain::Identity identity,
                          std::optional<std::string> operationGroupId,
                          std::function<void()> projectionCommitted) const
    {
        const bool creating = identity.id.empty();
        if (const auto validation = validateIdentity(identity, creating))
            co_return *validation;
        auto resolved = resolveContext(m_connection, std::move(settings), std::move(ownerAccountId),
                                       std::move(accountId));
        if (const auto* contextError = std::get_if<OperationError>(&resolved))
            co_return *contextError;
        const auto& context = std::get<ResolvedContext>(resolved);
        cache::IdentityRepository repository{m_connection};
        IdentityMutationJournal journal{m_connection, repository};
        auto active = journal.listActive(context.accountId);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&active))
            co_return operationError(*cacheError);
        if (!std::get<std::vector<IdentityMutationRecord>>(active).empty())
        {
            co_return error(OperationErrorCode::Conflict,
                            QStringLiteral("Another sender Identity change is still unresolved."));
        }

        const auto stateResult = repository.state(context.accountId);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&stateResult))
            co_return operationError(*cacheError);
        const auto baseState = std::get<std::optional<std::string>>(stateResult);
        const auto mutationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        const auto creationId = creating
                                    ? std::optional<std::string>{QUuid::createUuid()
                                                                     .toString(QUuid::WithoutBraces)
                                                                     .toStdString()}
                                    : std::nullopt;
        std::optional<javelin::jmap::domain::Identity> before;
        if (!creating)
        {
            auto found = repository.find(context.accountId, identity.id);
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&found))
                co_return operationError(*cacheError);
            before = std::get<std::optional<javelin::jmap::domain::Identity>>(found);
            if (!before.has_value())
                co_return error(OperationErrorCode::NotFound,
                                QStringLiteral("The sender Identity is no longer available."));
            if (before->email != identity.email)
                co_return error(
                    OperationErrorCode::InvalidUserInput,
                    QStringLiteral("A sender Identity email address cannot be changed."));
            identity.mayDelete = before->mayDelete;
        }
        else
            identity.mayDelete = true;

        IdentityMutationRecord mutation{
            .mutationId = mutationId,
            .operationGroupId = std::move(operationGroupId),
            .accountId = context.accountId,
            .objectId = creating ? *creationId : identity.id,
            .creationId = creationId,
            .kind = creating ? IdentityMutationKind::Create : IdentityMutationKind::Update,
            .status = sync::MutationStatus::Pending,
            .before = before,
            .after = identity,
            .baseState = baseState,
            .acceptedState = std::nullopt,
            .errorJson = std::nullopt,
        };
        if (const auto cacheError = journal.queue(mutation))
            co_return operationError(*cacheError);
        if (projectionCommitted)
            projectionCommitted();
        if (const auto cacheError = journal.transition(mutation, sync::MutationStatus::InFlight))
            co_return operationError(*cacheError);

        api::IdentitySetRequest request{
            .accountId = context.accountId,
            .ifInState = baseState,
            .create = {},
            .update = {},
            .destroy = {},
        };
        if (creating)
        {
            request.create.emplace(
                *creationId, api::IdentitySetCreate{
                                 .name = identity.name,
                                 .email = identity.email,
                                 .replyTo = identity.replyTo,
                                 .bcc = identity.bcc,
                                 .textSignature = identity.textSignature.value_or(std::string{}),
                                 .htmlSignature = identity.htmlSignature.value_or(std::string{}),
                             });
        }
        else
        {
            request.update.emplace(
                identity.id, api::IdentitySetUpdate{
                                 .name = identity.name,
                                 .replyTo = identity.replyTo,
                                 .bcc = identity.bcc,
                                 .textSignature = identity.textSignature.value_or(std::string{}),
                                 .htmlSignature = identity.htmlSignature.value_or(std::string{}),
                             });
        }

        bool dispatched = false;
        auto called = co_await setAndFetch(m_methodTransport, context, request, dispatched);
        if (const auto* callError = std::get_if<OperationError>(&called))
        {
            const bool deterministicMethodError = callError->protocolType.has_value();
            const auto cacheError =
                (!dispatched || deterministicMethodError)
                    ? journal.reject(mutation, std::nullopt, callError->protocolType)
                    : journal.transition(mutation, sync::MutationStatus::Unknown);
            if (cacheError.has_value())
                co_return operationError(*cacheError);
            co_return *callError;
        }
        auto response = std::get<SetAndGetResponse>(std::move(called));
        if (response.set.accountId != context.accountId)
        {
            if (const auto cacheError = journal.transition(mutation, sync::MutationStatus::Unknown))
                co_return operationError(*cacheError);
            co_return error(
                OperationErrorCode::ProtocolViolation,
                QStringLiteral("The server returned an Identity response for another account."));
        }

        if (creating)
        {
            if (const auto rejected = response.set.notCreated.find(*creationId);
                rejected != response.set.notCreated.end())
            {
                if (const auto cacheError =
                        journal.reject(mutation, std::nullopt, rejected->second.type))
                    co_return operationError(*cacheError);
                co_return setError(rejected->second,
                                   QStringLiteral("The server rejected the new sender Identity."));
            }
            if (!response.set.created.contains(*creationId) ||
                response.set.created.at(*creationId).empty())
            {
                if (const auto cacheError =
                        journal.transition(mutation, sync::MutationStatus::Unknown))
                    co_return operationError(*cacheError);
                co_return error(OperationErrorCode::ProtocolViolation,
                                QStringLiteral("The server did not return the new Identity id."));
            }
        }
        else
        {
            if (const auto rejected = response.set.notUpdated.find(identity.id);
                rejected != response.set.notUpdated.end())
            {
                if (const auto cacheError =
                        journal.reject(mutation, std::nullopt, rejected->second.type))
                    co_return operationError(*cacheError);
                co_return setError(rejected->second,
                                   QStringLiteral("The server rejected the Identity update."));
            }
            if (std::ranges::find(response.set.updated, identity.id) == response.set.updated.end())
            {
                if (const auto cacheError =
                        journal.transition(mutation, sync::MutationStatus::Unknown))
                    co_return operationError(*cacheError);
                co_return error(OperationErrorCode::ProtocolViolation,
                                QStringLiteral("The server did not confirm the Identity update."));
            }
        }

        auto confirmed = response.get.has_value()
                             ? response.get->list
                             : fallbackAcceptedIdentities(repository, mutation, response.set);
        const auto acceptedState = response.get.has_value()
                                       ? std::optional<std::string>{response.get->state}
                                       : response.set.newState;
        if (!acceptedState.has_value())
        {
            if (const auto cacheError = journal.transition(mutation, sync::MutationStatus::Unknown))
                co_return operationError(*cacheError);
            co_return error(OperationErrorCode::ProtocolViolation,
                            QStringLiteral("The Identity/set response omitted its new state."));
        }
        if (confirmed.empty())
        {
            if (const auto cacheError = journal.transition(mutation, sync::MutationStatus::Unknown))
                co_return operationError(*cacheError);
            co_return error(OperationErrorCode::LocalStorageFailure,
                            QStringLiteral("The accepted Identity could not be materialized."));
        }
        if (const auto cacheError = journal.accept(mutation, confirmed, *acceptedState))
            co_return operationError(*cacheError);

        const auto savedId = creating ? response.set.created.at(*creationId) : identity.id;
        const auto saved = findIdentity(confirmed, savedId);
        if (!saved.has_value())
            co_return error(
                OperationErrorCode::ProtocolViolation,
                QStringLiteral("The accepted Identity is missing from the server snapshot."));
        co_return *saved;
    }

    QCoro::Task<IdentityDeleteResult>
    IdentityService::remove(LiveConnectionSettings settings, std::string accountId,
                            std::string identityId, std::optional<std::string> operationGroupId,
                            std::function<void()> projectionCommitted) const
    {
        auto ownerAccountId = accountId;
        co_return co_await remove(std::move(settings), std::move(ownerAccountId),
                                  std::move(accountId), std::move(identityId),
                                  std::move(operationGroupId), std::move(projectionCommitted));
    }

    QCoro::Task<IdentityDeleteResult>
    IdentityService::remove(LiveConnectionSettings settings, std::string ownerAccountId,
                            std::string accountId, std::string identityId,
                            std::optional<std::string> operationGroupId,
                            std::function<void()> projectionCommitted) const
    {
        auto resolved = resolveContext(m_connection, std::move(settings), std::move(ownerAccountId),
                                       std::move(accountId));
        if (const auto* contextError = std::get_if<OperationError>(&resolved))
            co_return *contextError;
        const auto& context = std::get<ResolvedContext>(resolved);
        cache::IdentityRepository repository{m_connection};
        IdentityMutationJournal journal{m_connection, repository};
        auto active = journal.listActive(context.accountId);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&active))
            co_return operationError(*cacheError);
        if (!std::get<std::vector<IdentityMutationRecord>>(active).empty())
        {
            co_return error(OperationErrorCode::Conflict,
                            QStringLiteral("Another sender Identity change is still unresolved."));
        }
        auto found = repository.find(context.accountId, identityId);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&found))
            co_return operationError(*cacheError);
        const auto before =
            std::get<std::optional<javelin::jmap::domain::Identity>>(std::move(found));
        if (!before.has_value())
            co_return error(OperationErrorCode::NotFound,
                            QStringLiteral("The sender Identity is no longer available."));
        if (!before->mayDelete)
            co_return error(
                OperationErrorCode::PermissionDenied,
                QStringLiteral("The server does not allow this Identity to be deleted."));
        const auto stateResult = repository.state(context.accountId);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&stateResult))
            co_return operationError(*cacheError);
        const auto baseState = std::get<std::optional<std::string>>(stateResult);
        IdentityMutationRecord mutation{
            .mutationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
            .operationGroupId = std::move(operationGroupId),
            .accountId = context.accountId,
            .objectId = identityId,
            .creationId = std::nullopt,
            .kind = IdentityMutationKind::Destroy,
            .status = sync::MutationStatus::Pending,
            .before = before,
            .after = std::nullopt,
            .baseState = baseState,
            .acceptedState = std::nullopt,
            .errorJson = std::nullopt,
        };
        if (const auto cacheError = journal.queue(mutation))
            co_return operationError(*cacheError);
        if (projectionCommitted)
            projectionCommitted();
        if (const auto cacheError = journal.transition(mutation, sync::MutationStatus::InFlight))
            co_return operationError(*cacheError);

        api::IdentitySetRequest request{
            .accountId = context.accountId,
            .ifInState = baseState,
            .create = {},
            .update = {},
            .destroy = {identityId},
        };
        bool dispatched = false;
        auto called = co_await setAndFetch(m_methodTransport, context, request, dispatched);
        if (const auto* callError = std::get_if<OperationError>(&called))
        {
            const bool deterministicMethodError = callError->protocolType.has_value();
            const auto cacheError =
                (!dispatched || deterministicMethodError)
                    ? journal.reject(mutation, std::nullopt, callError->protocolType)
                    : journal.transition(mutation, sync::MutationStatus::Unknown);
            if (cacheError.has_value())
                co_return operationError(*cacheError);
            co_return *callError;
        }
        auto response = std::get<SetAndGetResponse>(std::move(called));
        if (const auto rejected = response.set.notDestroyed.find(identityId);
            rejected != response.set.notDestroyed.end())
        {
            if (const auto cacheError =
                    journal.reject(mutation, std::nullopt, rejected->second.type))
                co_return operationError(*cacheError);
            co_return setError(rejected->second,
                               QStringLiteral("The server rejected the Identity deletion."));
        }
        if (std::ranges::find(response.set.destroyed, identityId) == response.set.destroyed.end())
        {
            if (const auto cacheError = journal.transition(mutation, sync::MutationStatus::Unknown))
                co_return operationError(*cacheError);
            co_return error(OperationErrorCode::ProtocolViolation,
                            QStringLiteral("The server did not confirm the Identity deletion."));
        }
        auto confirmed = response.get.has_value()
                             ? response.get->list
                             : fallbackAcceptedIdentities(repository, mutation, response.set);
        const auto acceptedState = response.get.has_value()
                                       ? std::optional<std::string>{response.get->state}
                                       : response.set.newState;
        if (!acceptedState.has_value())
        {
            if (const auto cacheError = journal.transition(mutation, sync::MutationStatus::Unknown))
                co_return operationError(*cacheError);
            co_return error(OperationErrorCode::ProtocolViolation,
                            QStringLiteral("The Identity/set response omitted its new state."));
        }
        if (const auto cacheError = journal.accept(mutation, confirmed, *acceptedState))
            co_return operationError(*cacheError);
        co_return std::monostate{};
    }
} // namespace javelin::jmap::identity
