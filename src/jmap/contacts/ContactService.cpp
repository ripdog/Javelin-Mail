#include "jmap/contacts/ContactService.h"

#include "jmap/api/Error.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/PatchObject.h"
#include "jmap/api/RequestBuilder.h"
#include "jmap/api/Session.h"
#include "jmap/api/Transport.h"
#include "jmap/auth/Auth.h"
#include "jmap/cache/ContactRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/contacts/AddressBookMutationJournal.h"
#include "jmap/contacts/ContactMutationJournal.h"
#include "jmap/contacts/ContactTypes.h"
#include "jmap/sync/ConsistencyDomain.h"
#include "jmap/sync/MutationJournal.h"

#include <glaze/glaze.hpp>

#include <QStringList>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <unordered_set>
#include <utility>

namespace javelin::jmap::contacts::detail
{
    struct UploadResponse
    {
        std::string accountId;
        std::string blobId;
        std::string type;
        std::uint64_t size = 0;
    };

    struct CreatedObject
    {
        std::string id;
    };
} // namespace javelin::jmap::contacts::detail

template <> struct glz::meta<javelin::jmap::contacts::detail::UploadResponse>
{
    using T = javelin::jmap::contacts::detail::UploadResponse;
    static constexpr auto value = glz::object("accountId", &T::accountId, "blobId", &T::blobId,
                                              "type", &T::type, "size", &T::size);
};

template <> struct glz::meta<javelin::jmap::contacts::detail::CreatedObject>
{
    using T = javelin::jmap::contacts::detail::CreatedObject;
    static constexpr auto value = glz::object("id", &T::id);
};

namespace javelin::jmap::contacts
{
    namespace
    {
        using SessionResult =
            std::variant<javelin::jmap::api::Session, javelin::jmap::OperationError>;

        struct MethodFailure
        {
            javelin::jmap::api::MethodError error;
        };

        using ContactMethodResult = std::variant<javelin::jmap::api::MethodInvocation,
                                                 MethodFailure, javelin::jmap::OperationError>;

        struct CannotCalculateChanges
        {
        };

        struct SupersededRefresh
        {
        };

        struct AccountRefreshSummary
        {
            std::size_t addressBookCount = 0;
            std::size_t contactCount = 0;
        };

        using IncrementalRefreshResult =
            std::variant<AccountRefreshSummary, CannotCalculateChanges, SupersededRefresh,
                         javelin::jmap::OperationError>;

        struct ContactGetBatch
        {
            std::vector<ContactSummary> contacts;
            std::vector<std::string> notFound;
            bool stateAdvanced = false;
        };

        using ContactGetBatchResult = std::variant<ContactGetBatch, javelin::jmap::OperationError>;

        [[nodiscard]] javelin::jmap::OperationError
        error(QString message, const javelin::jmap::OperationErrorCode code =
                                   javelin::jmap::OperationErrorCode::ServerFailure)
        {
            return {.code = code, .message = std::move(message)};
        }

        [[nodiscard]] std::variant<javelin::jmap::sync::RefreshFence, javelin::jmap::OperationError>
        captureFence(javelin::jmap::cache::DatabaseConnection& connection,
                     const std::string_view accountId, const std::string_view dataType)
        {
            javelin::jmap::sync::ConsistencyDomainRepository repository{connection};
            const auto result = repository.captureRefresh(
                {.accountId = std::string{accountId}, .dataType = std::string{dataType}});
            if (const auto* cacheError = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                return error(cacheError->message,
                             javelin::jmap::OperationErrorCode::LocalStorageFailure);
            return std::get<javelin::jmap::sync::RefreshFence>(result);
        }

        [[nodiscard]] std::variant<bool, javelin::jmap::OperationError>
        fenceIsCurrent(javelin::jmap::cache::DatabaseConnection& connection,
                       const javelin::jmap::sync::RefreshFence& fence)
        {
            javelin::jmap::sync::ConsistencyDomainRepository repository{connection};
            const auto result = repository.canCommitRefresh(fence);
            if (const auto* cacheError = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                return error(cacheError->message,
                             javelin::jmap::OperationErrorCode::LocalStorageFailure);
            return std::get<bool>(result);
        }

        [[nodiscard]] std::variant<bool, javelin::jmap::OperationError>
        fenceGenerationIsCurrent(javelin::jmap::cache::DatabaseConnection& connection,
                                 const javelin::jmap::sync::RefreshFence& fence)
        {
            javelin::jmap::sync::ConsistencyDomainRepository repository{connection};
            const auto result = repository.isCurrent(fence);
            if (const auto* cacheError = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                return error(cacheError->message,
                             javelin::jmap::OperationErrorCode::LocalStorageFailure);
            return std::get<bool>(result);
        }

        [[nodiscard]] SessionResult
        loadSession(javelin::jmap::cache::DatabaseConnection& connection,
                    const std::string_view ownerAccountId)
        {
            javelin::jmap::cache::SessionRepository repository{connection};
            const auto result = repository.load(ownerAccountId);
            if (const auto* databaseError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            {
                return javelin::jmap::operationError(*databaseError);
            }
            const auto& session = std::get<std::optional<javelin::jmap::api::Session>>(result);
            if (!session.has_value())
            {
                return error(QStringLiteral("No cached JMAP session is available."),
                             javelin::jmap::OperationErrorCode::PreconditionFailed);
            }
            return *session;
        }

        [[nodiscard]] javelin::jmap::auth::AccountCredentials
        credentials(const javelin::jmap::LiveConnectionSettings& settings, std::string accountId)
        {
            return {.accountId = std::move(accountId),
                    .emailAddress = settings.loginEmail,
                    .sessionUrl = settings.sessionUrl,
                    .token = {.accessToken = settings.apiKey,
                              .refreshToken = std::nullopt,
                              .expiry = std::nullopt}};
        }

        [[nodiscard]] javelin::jmap::api::ApiRequestContext
        context(const javelin::jmap::LiveConnectionSettings& settings,
                const javelin::jmap::api::Session& session, std::string accountId)
        {
            return {.credentials = credentials(settings, std::move(accountId)),
                    .apiUrl = session.apiUrl,
                    .requestLimits = javelin::jmap::api::coreRequestLimits(session)};
        }

        [[nodiscard]] std::optional<javelin::jmap::api::MethodInvocation>
        response(const javelin::jmap::api::ResponseEnvelope& envelope,
                 const std::string_view callId, const std::string_view expectedName)
        {
            for (const auto& item : envelope.methodResponses)
            {
                if (item.callId == callId && item.name == expectedName)
                {
                    return item;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] javelin::jmap::OperationError
        callError(const javelin::jmap::api::MethodCallerResult& result)
        {
            if (const auto* transport = std::get_if<javelin::jmap::api::TransportError>(&result))
            {
                return javelin::jmap::operationError(*transport);
            }
            if (const auto* auth = std::get_if<javelin::jmap::api::AuthError>(&result))
            {
                return javelin::jmap::operationError(*auth);
            }
            if (const auto* protocol = std::get_if<javelin::jmap::api::ProtocolError>(&result))
            {
                return javelin::jmap::operationError(*protocol);
            }
            return error(QStringLiteral("Unknown Contacts request failure."));
        }

        [[nodiscard]] QCoro::Task<ContactMethodResult>
        callContactMethod(javelin::jmap::api::JmapMethodTransport& methodTransport,
                          const javelin::jmap::LiveConnectionSettings& settings,
                          const javelin::jmap::api::Session& session, const std::string& accountId,
                          std::string methodName, std::string arguments)
        {
            const std::string expectedName = methodName;
            javelin::jmap::api::RequestBuilder builder;
            builder.useCore().useCapability(std::string{javelin::jmap::api::contactsCapabilityUri});
            static_cast<void>(builder.call(
                javelin::jmap::api::MethodRequest<javelin::jmap::api::MethodInvocation>{
                    .name = std::move(methodName), .arguments = std::move(arguments)},
                "contacts-method"));
            javelin::jmap::api::MethodCaller caller{methodTransport};
            const auto callResult =
                co_await caller.call(context(settings, session, accountId), builder);
            const auto* envelope = std::get_if<javelin::jmap::api::ResponseEnvelope>(&callResult);
            if (envelope == nullptr)
                co_return callError(callResult);

            for (const auto& invocation : envelope->methodResponses)
            {
                if (invocation.callId != "contacts-method")
                    continue;
                if (invocation.name == expectedName)
                    co_return invocation;
                if (invocation.name == "error")
                {
                    const auto parsed = javelin::jmap::api::parseMethodError(invocation.arguments);
                    if (!parsed.ok())
                    {
                        co_return error(QStringLiteral("Invalid Contacts method error response."));
                    }
                    co_return MethodFailure{.error = *parsed.value};
                }
            }
            co_return error(QStringLiteral("The Contacts response omitted the requested method."));
        }

        [[nodiscard]] QCoro::Task<ContactGetBatchResult>
        fetchContactCards(javelin::jmap::api::JmapMethodTransport& methodTransport,
                          const javelin::jmap::LiveConnectionSettings& settings,
                          const javelin::jmap::api::Session& session, const std::string& accountId,
                          const std::vector<std::string>& ids, const std::string_view expectedState)
        {
            ContactGetBatch result;
            const std::uint64_t advertisedLimit =
                session.capabilities.coreDetails.has_value()
                    ? session.capabilities.coreDetails->maxObjectsInGet.value_or(ids.size())
                    : ids.size();
            const auto batchSize = static_cast<std::size_t>(std::max<std::uint64_t>(
                1,
                std::min<std::uint64_t>(advertisedLimit, std::numeric_limits<std::size_t>::max())));
            for (std::size_t offset = 0; offset < ids.size(); offset += batchSize)
            {
                const auto end = std::min(ids.size(), offset + batchSize);
                const std::vector batchIds(ids.begin() + static_cast<std::ptrdiff_t>(offset),
                                           ids.begin() + static_cast<std::ptrdiff_t>(end));
                const auto arguments =
                    javelin::jmap::api::serializeGetRequest({.accountId = accountId,
                                                             .ids = batchIds,
                                                             .idsReference = std::nullopt,
                                                             .properties = std::nullopt});
                if (!arguments.has_value())
                    co_return error(QStringLiteral("Unable to serialize ContactCard/get."));
                auto callResult = co_await callContactMethod(
                    methodTransport, settings, session, accountId, "ContactCard/get", *arguments);
                if (const auto* callFailure =
                        std::get_if<javelin::jmap::OperationError>(&callResult))
                    co_return *callFailure;
                if (const auto* methodFailure = std::get_if<MethodFailure>(&callResult))
                    co_return javelin::jmap::operationError(methodFailure->error);
                const auto parsed = javelin::jmap::api::parseContactCardGetResponse(
                    std::get<javelin::jmap::api::MethodInvocation>(callResult).arguments);
                if (!parsed.ok() || parsed.value->accountId != accountId)
                    co_return error(QStringLiteral("Unable to parse ContactCard/get response."));
                result.stateAdvanced = result.stateAdvanced || parsed.value->state != expectedState;
                std::unordered_set<std::string> requested(batchIds.begin(), batchIds.end());
                std::unordered_set<std::string> accounted;
                for (const auto& card : parsed.value->list)
                {
                    if (!requested.contains(card.id) || !accounted.insert(card.id).second)
                    {
                        co_return error(
                            QStringLiteral("ContactCard/get returned an unexpected duplicate ID."),
                            javelin::jmap::OperationErrorCode::ProtocolViolation);
                    }
                    auto contact = summarizeContact(accountId, card);
                    if (!contact.has_value())
                        co_return error(
                            QStringLiteral("The server returned an invalid ContactCard."));
                    result.contacts.push_back(std::move(*contact));
                }
                for (const auto& id : parsed.value->notFound)
                {
                    if (!requested.contains(id) || !accounted.insert(id).second)
                    {
                        co_return error(
                            QStringLiteral(
                                "ContactCard/get returned an unexpected duplicate notFound ID."),
                            javelin::jmap::OperationErrorCode::ProtocolViolation);
                    }
                    result.notFound.push_back(id);
                }
                if (accounted.size() != requested.size())
                {
                    co_return error(
                        QStringLiteral("ContactCard/get omitted requested materialization IDs."),
                        javelin::jmap::OperationErrorCode::ProtocolViolation);
                }
            }
            co_return result;
        }

        [[nodiscard]] QCoro::Task<IncrementalRefreshResult>
        refreshAccountIncrementally(javelin::jmap::cache::ContactRepository& repository,
                                    javelin::jmap::cache::DatabaseConnection& connection,
                                    javelin::jmap::api::JmapMethodTransport& methodTransport,
                                    const javelin::jmap::LiveConnectionSettings& settings,
                                    const javelin::jmap::api::Session& session,
                                    const std::string& accountId, std::string state)
        {
            const auto addressBookFenceResult = captureFence(connection, accountId, "AddressBook");
            const auto contactFenceResult = captureFence(connection, accountId, "ContactCard");
            if (const auto* serviceError =
                    std::get_if<javelin::jmap::OperationError>(&addressBookFenceResult))
                co_return *serviceError;
            if (const auto* serviceError =
                    std::get_if<javelin::jmap::OperationError>(&contactFenceResult))
                co_return *serviceError;
            const auto addressBookFence =
                std::get<javelin::jmap::sync::RefreshFence>(addressBookFenceResult);
            const auto contactFence =
                std::get<javelin::jmap::sync::RefreshFence>(contactFenceResult);
            const auto booksArguments =
                javelin::jmap::api::serializeGetRequest({.accountId = accountId,
                                                         .ids = std::nullopt,
                                                         .idsReference = std::nullopt,
                                                         .properties = std::nullopt});
            if (!booksArguments.has_value())
                co_return error(QStringLiteral("Unable to serialize AddressBook/get."));
            auto booksCall = co_await callContactMethod(
                methodTransport, settings, session, accountId, "AddressBook/get", *booksArguments);
            if (const auto* callFailure = std::get_if<javelin::jmap::OperationError>(&booksCall))
                co_return *callFailure;
            if (const auto* methodFailure = std::get_if<MethodFailure>(&booksCall))
                co_return javelin::jmap::operationError(methodFailure->error);
            const auto books = javelin::jmap::api::parseAddressBookGetResponse(
                std::get<javelin::jmap::api::MethodInvocation>(booksCall).arguments);
            if (!books.ok())
                co_return error(QStringLiteral("Unable to parse AddressBook/get response."));
            const auto addressBooksCurrent = fenceIsCurrent(connection, addressBookFence);
            if (const auto* serviceError =
                    std::get_if<javelin::jmap::OperationError>(&addressBooksCurrent))
                co_return *serviceError;
            if (!std::get<bool>(addressBooksCurrent))
                co_return SupersededRefresh{};
            if (const auto cacheError = repository.replaceAddressBooks(accountId, books.value->list,
                                                                       books.value->state))
                co_return error(cacheError->message);

            bool hasMoreChanges = false;
            do
            {
                const auto changesArguments = javelin::jmap::api::serializeChangesRequest(
                    {.accountId = accountId, .sinceState = state, .maxChanges = std::nullopt});
                if (!changesArguments.has_value())
                    co_return error(QStringLiteral("Unable to serialize ContactCard/changes."));
                auto changesCall =
                    co_await callContactMethod(methodTransport, settings, session, accountId,
                                               "ContactCard/changes", *changesArguments);
                if (const auto* callFailure =
                        std::get_if<javelin::jmap::OperationError>(&changesCall))
                    co_return *callFailure;
                if (const auto* methodFailure = std::get_if<MethodFailure>(&changesCall))
                {
                    if (methodFailure->error.type == "cannotCalculateChanges")
                        co_return CannotCalculateChanges{};
                    co_return javelin::jmap::operationError(methodFailure->error);
                }
                const auto changes = javelin::jmap::api::parseChangesResponse(
                    std::get<javelin::jmap::api::MethodInvocation>(changesCall).arguments);
                if (!changes.ok() || changes.value->accountId != accountId ||
                    changes.value->oldState != state)
                    co_return error(QStringLiteral("Invalid ContactCard/changes response."));

                std::unordered_set<std::string> destroyed(changes.value->destroyed.begin(),
                                                          changes.value->destroyed.end());
                std::vector<std::string> changedIds;
                std::unordered_set<std::string> seenChangedIds;
                const auto collectChanged = [&](const std::vector<std::string>& ids)
                {
                    for (const auto& id : ids)
                    {
                        if (!destroyed.contains(id) && seenChangedIds.insert(id).second)
                            changedIds.push_back(id);
                    }
                };
                collectChanged(changes.value->created);
                collectChanged(changes.value->updated);
                auto fetched =
                    co_await fetchContactCards(methodTransport, settings, session, accountId,
                                               changedIds, changes.value->newState);
                if (const auto* fetchError = std::get_if<javelin::jmap::OperationError>(&fetched))
                    co_return *fetchError;
                auto batch = std::get<ContactGetBatch>(std::move(fetched));
                destroyed.insert(batch.notFound.begin(), batch.notFound.end());
                const std::vector<std::string> destroyedIds(destroyed.begin(), destroyed.end());
                const auto contactsCurrent = fenceIsCurrent(connection, contactFence);
                if (const auto* serviceError =
                        std::get_if<javelin::jmap::OperationError>(&contactsCurrent))
                    co_return *serviceError;
                if (!std::get<bool>(contactsCurrent))
                    co_return SupersededRefresh{};
                if (const auto cacheError = repository.upsertContacts(
                        accountId, batch.contacts, destroyedIds, changes.value->newState))
                    co_return error(cacheError->message);

                state = changes.value->newState;
                hasMoreChanges = changes.value->hasMoreChanges || batch.stateAdvanced;
            } while (hasMoreChanges);

            const auto cachedContacts = repository.listContacts(accountId);
            if (const auto* cacheError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&cachedContacts))
                co_return error(cacheError->message);
            co_return AccountRefreshSummary{
                .addressBookCount = books.value->list.size(),
                .contactCount = std::get<std::vector<ContactSummary>>(cachedContacts).size(),
            };
        }

        [[nodiscard]] std::optional<std::string>
        createdObjectId(const javelin::jmap::api::ContactDocument& document)
        {
            detail::CreatedObject object;
            if (glz::read<glz::opts{.error_on_unknown_keys = false}>(object, document.json) ||
                object.id.empty())
            {
                return std::nullopt;
            }
            return object.id;
        }

        [[nodiscard]] std::optional<std::string>
        createdId(const javelin::jmap::api::SetResult& result)
        {
            if (result.created.empty())
            {
                return std::nullopt;
            }
            return createdObjectId(result.created.begin()->second);
        }

        [[nodiscard]] std::vector<CreatedContactMapping>
        createdIds(const javelin::jmap::api::SetResult& result)
        {
            std::vector<CreatedContactMapping> mappings;
            mappings.reserve(result.created.size());
            for (const auto& [creationId, document] : result.created)
                if (auto serverId = createdObjectId(document))
                    mappings.push_back(
                        {.creationId = creationId, .serverId = std::move(*serverId)});
            std::ranges::sort(mappings, {}, &CreatedContactMapping::creationId);
            return mappings;
        }

        struct PreparedAddressBookMutations
        {
            std::vector<AddressBookMutationRecord> records;
            std::vector<javelin::jmap::api::AddressBook> projectedBooks;
        };

        [[nodiscard]]
        std::variant<PreparedAddressBookMutations, javelin::jmap::OperationError>
        prepareAddressBookMutations(javelin::jmap::cache::ContactRepository& repository,
                                    const javelin::jmap::api::AddressBookSetRequest& request)
        {
            const auto listed = repository.listAddressBooks(request.accountId);
            if (const auto* cacheError = std::get_if<javelin::jmap::cache::DatabaseError>(&listed))
                return error(cacheError->message,
                             javelin::jmap::OperationErrorCode::LocalStorageFailure);
            PreparedAddressBookMutations prepared{
                .records = {},
                .projectedBooks = std::get<std::vector<javelin::jmap::api::AddressBook>>(listed),
            };
            prepared.records.reserve(request.create.size() + request.update.size() +
                                     request.destroy.size());

            for (const auto& [creationId, document] : request.create)
            {
                const auto mutationId =
                    QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
                const auto temporaryId = "local-" + mutationId;
                const auto projected =
                    javelin::jmap::api::parseAddressBookDocument(temporaryId, document.json);
                if (!projected.ok() || !projected.value.has_value())
                    return error(QStringLiteral("The AddressBook creation document is invalid."),
                                 javelin::jmap::OperationErrorCode::InvalidRequest);
                const auto projectedDocument =
                    javelin::jmap::api::serializeAddressBookDocument(*projected.value);
                if (!projectedDocument.has_value())
                    return error(QStringLiteral("Unable to serialize the AddressBook projection."),
                                 javelin::jmap::OperationErrorCode::InvalidRequest);
                prepared.records.push_back({
                    .mutationId = mutationId,
                    .operationGroupId = std::nullopt,
                    .accountId = request.accountId,
                    .objectId = temporaryId,
                    .creationId = creationId,
                    .kind = AddressBookMutationKind::Create,
                    .status = javelin::jmap::sync::MutationStatus::Pending,
                    .requestedDocument = document.json,
                    .baseDocument = std::nullopt,
                    .projectedDocument = *projectedDocument,
                    .baseState = request.ifInState,
                    .acceptedState = std::nullopt,
                    .errorJson = std::nullopt,
                });
                prepared.projectedBooks.push_back(*projected.value);
            }
            for (const auto& [addressBookId, patch] : request.update)
            {
                const auto found = std::ranges::find(prepared.projectedBooks, addressBookId,
                                                     &javelin::jmap::api::AddressBook::id);
                std::optional<std::string> baseDocument;
                std::optional<std::string> projectedDocument;
                if (found != prepared.projectedBooks.end())
                {
                    baseDocument = javelin::jmap::api::serializeAddressBookDocument(*found);
                    if (!baseDocument.has_value())
                        return error(QStringLiteral("Unable to serialize the cached AddressBook."),
                                     javelin::jmap::OperationErrorCode::LocalStorageFailure);
                    auto applied = javelin::jmap::api::applyPatchObject(*baseDocument, patch.json);
                    const auto* json = std::get_if<std::string>(&applied);
                    if (json != nullptr)
                    {
                        auto projected =
                            javelin::jmap::api::parseAddressBookDocument(addressBookId, *json);
                        if (projected.ok() && projected.value.has_value())
                        {
                            *found = *projected.value;
                            projectedDocument = *json;
                        }
                    }
                }
                prepared.records.push_back({
                    .mutationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
                    .operationGroupId = std::nullopt,
                    .accountId = request.accountId,
                    .objectId = addressBookId,
                    .creationId = std::nullopt,
                    .kind = AddressBookMutationKind::Update,
                    .status = javelin::jmap::sync::MutationStatus::Pending,
                    .requestedDocument = patch.json,
                    .baseDocument = std::move(baseDocument),
                    .projectedDocument = std::move(projectedDocument),
                    .baseState = request.ifInState,
                    .acceptedState = std::nullopt,
                    .errorJson = std::nullopt,
                });
            }
            for (const auto& addressBookId : request.destroy)
            {
                const auto found = std::ranges::find(prepared.projectedBooks, addressBookId,
                                                     &javelin::jmap::api::AddressBook::id);
                std::optional<std::string> baseDocument;
                if (found != prepared.projectedBooks.end())
                {
                    baseDocument = javelin::jmap::api::serializeAddressBookDocument(*found);
                    prepared.projectedBooks.erase(found);
                }
                prepared.records.push_back({
                    .mutationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
                    .operationGroupId = std::nullopt,
                    .accountId = request.accountId,
                    .objectId = addressBookId,
                    .creationId = std::nullopt,
                    .kind = AddressBookMutationKind::Destroy,
                    .status = javelin::jmap::sync::MutationStatus::Pending,
                    .requestedDocument = "{}",
                    .baseDocument = std::move(baseDocument),
                    .projectedDocument = std::nullopt,
                    .baseState = request.ifInState,
                    .acceptedState = std::nullopt,
                    .errorJson = std::nullopt,
                });
            }
            return prepared;
        }

        struct PreparedContactMutations
        {
            std::vector<ContactMutationRecord> records;
            std::vector<ContactSummary> projectedContacts;
            std::vector<std::string> destroyedIds;
        };

        [[nodiscard]] std::string newMutationId()
        {
            return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        }

        struct PreparedContactCopy
        {
            std::vector<ContactMutationRecord> records;
            std::vector<ContactProjection> projections;
        };

        [[nodiscard]] std::variant<std::optional<std::string>, javelin::jmap::OperationError>
        contactState(javelin::jmap::cache::DatabaseConnection& connection,
                     const std::string& accountId)
        {
            javelin::jmap::cache::SyncStateRepository states{connection};
            const auto result =
                states.find({.accountId = accountId, .objectType = "ContactCard", .queryKey = {}});
            if (const auto* cacheError = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                return error(cacheError->message,
                             javelin::jmap::OperationErrorCode::LocalStorageFailure);
            const auto& state =
                std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(result);
            return state.has_value() ? std::optional<std::string>{state->stateToken} : std::nullopt;
        }

        [[nodiscard]] std::variant<PreparedContactCopy, javelin::jmap::OperationError>
        prepareContactCopy(javelin::jmap::cache::ContactRepository& repository,
                           const javelin::jmap::api::ContactCardCopyRequest& request)
        {
            if (request.fromAccountId == request.accountId)
                return error(QStringLiteral("ContactCard/copy requires two different accounts."),
                             javelin::jmap::OperationErrorCode::InvalidRequest);

            PreparedContactCopy prepared;
            const auto operationGroupId = newMutationId();
            ContactProjection destination{
                .accountId = request.accountId,
                .contacts = {},
                .destroyedIds = {},
            };
            ContactProjection source{
                .accountId = request.fromAccountId,
                .contacts = {},
                .destroyedIds = {},
            };
            for (const auto& [creationId, copyDocument] : request.create)
            {
                std::string copyBuffer = copyDocument.json;
                glz::generic overrides;
                if (glz::read_json(overrides, copyBuffer) || !overrides.is_object() ||
                    !overrides.contains("id") || !overrides.at("id").is_string())
                    return error(
                        QStringLiteral("Each ContactCard copy requires a source object id."),
                        javelin::jmap::OperationErrorCode::InvalidRequest);
                const auto sourceId = overrides.at("id").get_string();
                const auto found = repository.findContact(request.fromAccountId, sourceId);
                if (const auto* cacheError =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&found))
                    return error(cacheError->message,
                                 javelin::jmap::OperationErrorCode::LocalStorageFailure);
                const auto& sourceContact = std::get<std::optional<ContactSummary>>(found);
                if (!sourceContact.has_value())
                    return error(QStringLiteral("The source contact is not cached."),
                                 javelin::jmap::OperationErrorCode::NotFound);

                std::string projectedBuffer = sourceContact->document;
                glz::generic projected;
                if (glz::read_json(projected, projectedBuffer) || !projected.is_object())
                    return error(QStringLiteral("The cached source contact is invalid."),
                                 javelin::jmap::OperationErrorCode::LocalStorageFailure);
                overrides.get_object().erase("id");
                projected.get_object().erase("id");
                for (auto& [key, value] : overrides.get_object())
                    projected[key] = std::move(value);
                std::string projectedDocument;
                if (glz::write_json(projected, projectedDocument))
                    return error(QStringLiteral("Unable to materialize the copied contact."),
                                 javelin::jmap::OperationErrorCode::LocalStorageFailure);

                const auto mutationId = newMutationId();
                const auto temporaryId = std::string{"local-"} + mutationId;
                auto projectedContact =
                    summarizeContact(request.accountId, javelin::jmap::api::ContactCard{
                                                            .id = temporaryId,
                                                            .uid = {},
                                                            .kind = {},
                                                            .document = projectedDocument,
                                                        });
                if (!projectedContact.has_value())
                    return error(QStringLiteral("Unable to summarize the copied contact."),
                                 javelin::jmap::OperationErrorCode::LocalStorageFailure);
                destination.contacts.push_back(std::move(*projectedContact));
                prepared.records.push_back({
                    .mutationId = mutationId,
                    .operationGroupId = operationGroupId,
                    .accountId = request.accountId,
                    .objectId = temporaryId,
                    .creationId = creationId,
                    .kind = ContactMutationKind::Create,
                    .status = javelin::jmap::sync::MutationStatus::Pending,
                    .requestedDocument = copyDocument.json,
                    .baseDocument = std::nullopt,
                    .projectedDocument = std::move(projectedDocument),
                    .baseState = request.ifInState,
                    .acceptedState = std::nullopt,
                    .errorJson = std::nullopt,
                });
                if (request.onSuccessDestroyOriginal)
                {
                    source.destroyedIds.push_back(sourceId);
                    prepared.records.push_back({
                        .mutationId = newMutationId(),
                        .operationGroupId = operationGroupId,
                        .accountId = request.fromAccountId,
                        .objectId = sourceId,
                        .creationId = creationId,
                        .kind = ContactMutationKind::Destroy,
                        .status = javelin::jmap::sync::MutationStatus::Pending,
                        .requestedDocument = "{}",
                        .baseDocument = sourceContact->document,
                        .projectedDocument = std::nullopt,
                        .baseState = request.destroyFromIfInState,
                        .acceptedState = std::nullopt,
                        .errorJson = std::nullopt,
                    });
                }
            }
            prepared.projections.push_back(std::move(destination));
            if (request.onSuccessDestroyOriginal)
                prepared.projections.push_back(std::move(source));
            return prepared;
        }

        [[nodiscard]] std::variant<PreparedContactMutations, javelin::jmap::OperationError>
        prepareContactMutations(javelin::jmap::cache::ContactRepository& repository,
                                const javelin::jmap::api::ContactCardSetRequest& request)
        {
            PreparedContactMutations prepared;
            prepared.records.reserve(request.create.size() + request.update.size() +
                                     request.destroy.size());

            for (const auto& [creationId, document] : request.create)
            {
                const auto mutationId = newMutationId();
                const auto temporaryId = std::string{"local-"} + mutationId;
                std::optional<std::string> projectedDocument;
                if (auto projected =
                        summarizeContact(request.accountId, javelin::jmap::api::ContactCard{
                                                                .id = temporaryId,
                                                                .uid = {},
                                                                .kind = {},
                                                                .document = document.json,
                                                            }))
                {
                    projectedDocument = document.json;
                    prepared.projectedContacts.push_back(std::move(*projected));
                }
                prepared.records.push_back({
                    .mutationId = mutationId,
                    .operationGroupId = std::nullopt,
                    .accountId = request.accountId,
                    .objectId = temporaryId,
                    .creationId = creationId,
                    .kind = ContactMutationKind::Create,
                    .status = javelin::jmap::sync::MutationStatus::Pending,
                    .requestedDocument = document.json,
                    .baseDocument = std::nullopt,
                    .projectedDocument = std::move(projectedDocument),
                    .baseState = request.ifInState,
                    .acceptedState = std::nullopt,
                    .errorJson = std::nullopt,
                });
            }

            for (const auto& [contactId, patch] : request.update)
            {
                const auto cached = repository.findContact(request.accountId, contactId);
                if (const auto* cacheError =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&cached))
                {
                    return error(cacheError->message,
                                 javelin::jmap::OperationErrorCode::LocalStorageFailure);
                }
                const auto& base =
                    std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(cached);
                std::optional<std::string> projectedDocument;
                if (base.has_value())
                {
                    auto applied = javelin::jmap::api::applyPatchObject(base->document, patch.json);
                    if (auto* json = std::get_if<std::string>(&applied))
                    {
                        if (auto projected =
                                summarizeContact(request.accountId, javelin::jmap::api::ContactCard{
                                                                        .id = contactId,
                                                                        .uid = {},
                                                                        .kind = {},
                                                                        .document = *json,
                                                                    }))
                        {
                            projectedDocument = *json;
                            prepared.projectedContacts.push_back(std::move(*projected));
                        }
                    }
                }
                prepared.records.push_back({
                    .mutationId = newMutationId(),
                    .operationGroupId = std::nullopt,
                    .accountId = request.accountId,
                    .objectId = contactId,
                    .creationId = std::nullopt,
                    .kind = ContactMutationKind::Update,
                    .status = javelin::jmap::sync::MutationStatus::Pending,
                    .requestedDocument = patch.json,
                    .baseDocument = base.has_value() ? std::optional<std::string>{base->document}
                                                     : std::nullopt,
                    .projectedDocument = std::move(projectedDocument),
                    .baseState = request.ifInState,
                    .acceptedState = std::nullopt,
                    .errorJson = std::nullopt,
                });
            }

            for (const auto& contactId : request.destroy)
            {
                const auto cached = repository.findContact(request.accountId, contactId);
                if (const auto* cacheError =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&cached))
                {
                    return error(cacheError->message,
                                 javelin::jmap::OperationErrorCode::LocalStorageFailure);
                }
                const auto& base =
                    std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(cached);
                if (base.has_value())
                {
                    prepared.destroyedIds.push_back(contactId);
                }
                prepared.records.push_back({
                    .mutationId = newMutationId(),
                    .operationGroupId = std::nullopt,
                    .accountId = request.accountId,
                    .objectId = contactId,
                    .creationId = std::nullopt,
                    .kind = ContactMutationKind::Destroy,
                    .status = javelin::jmap::sync::MutationStatus::Pending,
                    .requestedDocument = "{}",
                    .baseDocument = base.has_value() ? std::optional<std::string>{base->document}
                                                     : std::nullopt,
                    .projectedDocument = std::nullopt,
                    .baseState = request.ifInState,
                    .acceptedState = std::nullopt,
                    .errorJson = std::nullopt,
                });
            }
            return prepared;
        }

        using SetObjectsResult =
            std::variant<javelin::jmap::api::SetResult, javelin::jmap::OperationError>;

        struct CopyObjectsResponse
        {
            javelin::jmap::api::SetResult copied;
            std::optional<javelin::jmap::api::SetResult> destroyedOriginals;
        };

        using CopyObjectsResult = std::variant<CopyObjectsResponse, javelin::jmap::OperationError>;

        [[nodiscard]] QString
        invocationSummary(const std::vector<javelin::jmap::api::MethodInvocation>& invocations)
        {
            QStringList summary;
            summary.reserve(static_cast<qsizetype>(invocations.size()));
            for (const auto& invocation : invocations)
                summary.push_back(
                    QStringLiteral("%1(%2)").arg(QString::fromStdString(invocation.name),
                                                 QString::fromStdString(invocation.callId)));
            return summary.isEmpty() ? QStringLiteral("<none>")
                                     : summary.join(QStringLiteral(", "));
        }

        [[nodiscard]] QCoro::Task<SetObjectsResult>
        setObjects(javelin::jmap::api::JmapMethodTransport& methodTransport,
                   javelin::jmap::cache::DatabaseConnection& connection,
                   javelin::jmap::LiveConnectionSettings settings, std::string ownerAccountId,
                   std::string accountId, std::string methodName,
                   std::optional<std::string> serialized)
        {
            if (!serialized.has_value())
            {
                co_return error(QStringLiteral("Unable to serialize the Contacts change."));
            }
            const auto sessionResult = loadSession(connection, ownerAccountId);
            if (const auto* loadError = std::get_if<javelin::jmap::OperationError>(&sessionResult))
            {
                co_return *loadError;
            }
            const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);
            const auto account = session.accounts.find(accountId);
            if (account == session.accounts.end() ||
                !account->second.accountCapabilities.contacts.has_value())
            {
                co_return error(QStringLiteral("This account does not support JMAP Contacts."),
                                javelin::jmap::OperationErrorCode::UnsupportedCapability);
            }

            const std::string expectedName = methodName;
            javelin::jmap::api::RequestBuilder builder;
            builder.useCore().useCapability(std::string{javelin::jmap::api::contactsCapabilityUri});
            static_cast<void>(builder.call(
                javelin::jmap::api::MethodRequest<javelin::jmap::api::SetResult>{
                    .name = std::move(methodName), .arguments = std::move(*serialized)},
                "contacts-set"));
            javelin::jmap::api::MethodCaller caller{methodTransport};
            const auto callResult =
                co_await caller.call(context(settings, session, accountId), builder);
            const auto* envelope = std::get_if<javelin::jmap::api::ResponseEnvelope>(&callResult);
            if (envelope == nullptr)
            {
                co_return callError(callResult);
            }
            std::optional<javelin::jmap::api::MethodInvocation> actual;
            for (const auto& item : envelope->methodResponses)
            {
                if (item.callId != "contacts-set")
                    continue;
                if (item.name == "error")
                {
                    const auto parsed = javelin::jmap::api::parseMethodError(item.arguments);
                    if (!parsed.ok())
                        co_return error(QStringLiteral("Invalid Contacts set error response."),
                                        javelin::jmap::OperationErrorCode::ProtocolViolation);
                    co_return javelin::jmap::operationError(*parsed.value);
                }
                if (item.name == expectedName)
                    actual = item;
            }
            if (!actual.has_value())
                co_return error(
                    QStringLiteral("The Contacts response omitted %1(contacts-set); received %2.")
                        .arg(QString::fromStdString(expectedName),
                             invocationSummary(envelope->methodResponses)),
                    javelin::jmap::OperationErrorCode::ProtocolViolation);
            const auto parsed = javelin::jmap::api::parseContactsSetResponse(actual->arguments);
            if (!parsed.ok())
            {
                co_return error(QStringLiteral("Invalid Contacts set response: %1")
                                    .arg(QString::fromStdString(parsed.error.value_or("unknown"))));
            }
            co_return *parsed.value;
        }

        [[nodiscard]] QCoro::Task<CopyObjectsResult>
        copyObjects(javelin::jmap::api::JmapMethodTransport& methodTransport,
                    javelin::jmap::cache::DatabaseConnection& connection,
                    javelin::jmap::LiveConnectionSettings settings, std::string ownerAccountId,
                    const javelin::jmap::api::ContactCardCopyRequest& request,
                    const std::string& serialized)
        {
            const auto sessionResult = loadSession(connection, ownerAccountId);
            if (const auto* loadError = std::get_if<javelin::jmap::OperationError>(&sessionResult))
                co_return *loadError;
            const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);
            for (const auto& accountId : {request.fromAccountId, request.accountId})
            {
                const auto account = session.accounts.find(accountId);
                if (account == session.accounts.end() ||
                    !account->second.accountCapabilities.contacts.has_value())
                    co_return error(
                        QStringLiteral("A copy account does not support JMAP Contacts."),
                        javelin::jmap::OperationErrorCode::UnsupportedCapability);
            }

            javelin::jmap::api::RequestBuilder builder;
            builder.useCore().useCapability(std::string{javelin::jmap::api::contactsCapabilityUri});
            static_cast<void>(builder.call(
                javelin::jmap::api::MethodRequest<javelin::jmap::api::SetResult>{
                    .name = "ContactCard/copy", .arguments = serialized},
                "contacts-copy"));
            javelin::jmap::api::MethodCaller caller{methodTransport};
            const auto callResult =
                co_await caller.call(context(settings, session, request.accountId), builder);
            const auto* envelope = std::get_if<javelin::jmap::api::ResponseEnvelope>(&callResult);
            if (envelope == nullptr)
                co_return callError(callResult);

            CopyObjectsResponse responseValue;
            bool foundCopy = false;
            for (const auto& invocation : envelope->methodResponses)
            {
                if (invocation.callId != "contacts-copy")
                    continue;
                if (invocation.name == "error")
                {
                    const auto parsed = javelin::jmap::api::parseMethodError(invocation.arguments);
                    if (!parsed.ok())
                        co_return error(QStringLiteral("Invalid ContactCard/copy error response."),
                                        javelin::jmap::OperationErrorCode::ProtocolViolation);
                    co_return javelin::jmap::operationError(*parsed.value);
                }
                if (invocation.name != "ContactCard/copy" && invocation.name != "ContactCard/set")
                    continue;
                const auto parsed =
                    javelin::jmap::api::parseContactsSetResponse(invocation.arguments);
                if (!parsed.ok())
                    co_return error(
                        QStringLiteral("Invalid Contacts copy response: %1")
                            .arg(QString::fromStdString(parsed.error.value_or("unknown"))),
                        javelin::jmap::OperationErrorCode::ProtocolViolation);
                if (invocation.name == "ContactCard/copy")
                {
                    responseValue.copied = std::move(*parsed.value);
                    foundCopy = true;
                }
                else
                    responseValue.destroyedOriginals = std::move(*parsed.value);
            }
            if (!foundCopy)
                co_return error(QStringLiteral("The server omitted the ContactCard/copy response."),
                                javelin::jmap::OperationErrorCode::ProtocolViolation);
            co_return responseValue;
        }

        [[nodiscard]] bool hasSetFailures(const javelin::jmap::api::SetResult& result)
        {
            return !result.notCreated.empty() || !result.notUpdated.empty() ||
                   !result.notDestroyed.empty();
        }

        [[nodiscard]] bool hasSetSuccesses(const javelin::jmap::api::SetResult& result)
        {
            return !result.created.empty() || !result.updated.empty() || !result.destroyed.empty();
        }

        [[nodiscard]] javelin::jmap::sync::MutationCommitReceipt
        contactReceipt(const javelin::jmap::api::SetResult& result, const std::string_view dataType)
        {
            javelin::jmap::sync::MutationCommitReceipt receipt{
                .domains =
                    {
                        {
                            .accountId = result.accountId,
                            .dataType = std::string{dataType},
                            .oldState = result.oldState,
                            .newState = result.newState,
                        },
                    },
                .acceptedObjectIds = result.destroyed,
                .rejectedObjectIds = {},
                .affectedCacheViews = {"contacts"},
                .incompleteMaterialization = false,
            };
            for (const auto& [id, document] : result.updated)
            {
                static_cast<void>(document);
                receipt.acceptedObjectIds.push_back(id);
            }
            for (const auto& mapping : createdIds(result))
                receipt.acceptedObjectIds.push_back(mapping.serverId);
            for (const auto& failures :
                 {&result.notCreated, &result.notUpdated, &result.notDestroyed})
                for (const auto& [id, document] : *failures)
                {
                    static_cast<void>(document);
                    receipt.rejectedObjectIds.push_back(id);
                }
            return receipt;
        }

        [[nodiscard]] ContactMutationResult
        commitSetResult(javelin::jmap::cache::DatabaseConnection& connection,
                        const javelin::jmap::api::SetResult& result,
                        const std::span<const javelin::jmap::sync::ConsistencyDomain> domains)
        {
            auto transactionResult = javelin::jmap::sync::MutationProjectionTransaction::begin(
                connection, QStringLiteral("Apply Contacts mutation response"));
            if (const auto* cacheError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            {
                return error(cacheError->message,
                             javelin::jmap::OperationErrorCode::LocalStorageFailure);
            }
            auto transaction = std::get<javelin::jmap::sync::MutationProjectionTransaction>(
                std::move(transactionResult));
            if (hasSetSuccesses(result))
            {
                if (const auto cacheError = transaction.advance(domains))
                    return error(cacheError->message,
                                 javelin::jmap::OperationErrorCode::LocalStorageFailure);
            }
            if (const auto cacheError = transaction.commit())
                return error(cacheError->message,
                             javelin::jmap::OperationErrorCode::LocalStorageFailure);
            if (hasSetFailures(result))
            {
                return error(QStringLiteral("The server rejected one or more Contacts changes."),
                             javelin::jmap::OperationErrorCode::Conflict);
            }
            return ContactMutationSummary{.accountId = result.accountId,
                                          .newState = result.newState,
                                          .createdId = createdId(result),
                                          .createdIds = createdIds(result),
                                          .receipt =
                                              contactReceipt(result, domains.front().dataType)};
        }

        [[nodiscard]] ContactMutationResult
        reconcileAddressBookMutations(javelin::jmap::cache::DatabaseConnection& connection,
                                      javelin::jmap::cache::ContactRepository& repository,
                                      const std::vector<AddressBookMutationRecord>& records,
                                      const javelin::jmap::api::SetResult& result)
        {
            const auto listed = repository.listAddressBooks(result.accountId);
            if (const auto* cacheError = std::get_if<javelin::jmap::cache::DatabaseError>(&listed))
                return error(cacheError->message,
                             javelin::jmap::OperationErrorCode::LocalStorageFailure);
            auto books = std::get<std::vector<javelin::jmap::api::AddressBook>>(listed);
            struct RejectedMutation
            {
                const AddressBookMutationRecord* record;
                std::string errorJson;
            };
            std::vector<const AddressBookMutationRecord*> acceptedRecords;
            std::vector<RejectedMutation> rejectedRecords;
            const auto eraseBook = [&books](const std::string_view id)
            {
                std::erase_if(books, [id](const javelin::jmap::api::AddressBook& book)
                              { return book.id == id; });
            };
            const auto restoreBook = [&books, &eraseBook](const AddressBookMutationRecord& record,
                                                          const std::string_view document)
                -> std::optional<javelin::jmap::OperationError>
            {
                const auto parsed =
                    javelin::jmap::api::parseAddressBookDocument(record.objectId, document);
                if (!parsed.ok() || !parsed.value.has_value())
                    return error(QStringLiteral("An AddressBook rollback document is invalid."),
                                 javelin::jmap::OperationErrorCode::LocalStorageFailure);
                eraseBook(record.objectId);
                books.push_back(*parsed.value);
                return std::nullopt;
            };

            for (const auto& record : records)
            {
                if (record.kind == AddressBookMutationKind::Create)
                {
                    if (!record.creationId.has_value())
                        return error(
                            QStringLiteral("An AddressBook creation lost its creation id."));
                    const auto created = result.created.find(*record.creationId);
                    if (created == result.created.end())
                    {
                        const auto rejected = result.notCreated.find(*record.creationId);
                        if (rejected == result.notCreated.end())
                            return error(
                                QStringLiteral("The AddressBook/set response omitted a creation."));
                        rejectedRecords.push_back(
                            {.record = &record, .errorJson = rejected->second.json});
                        eraseBook(record.objectId);
                        continue;
                    }
                    const auto serverId = createdObjectId(created->second);
                    if (!serverId.has_value() || !record.projectedDocument.has_value())
                        return error(QStringLiteral(
                            "The AddressBook/set response omitted the created object id."));
                    const auto transformed = javelin::jmap::api::applyPatchObject(
                        *record.projectedDocument, created->second.json);
                    const auto* document = std::get_if<std::string>(&transformed);
                    if (document == nullptr)
                        return error(
                            QStringLiteral("The AddressBook creation transformation is invalid."));
                    const auto parsed =
                        javelin::jmap::api::parseAddressBookDocument(*serverId, *document);
                    if (!parsed.ok() || !parsed.value.has_value())
                        return error(
                            QStringLiteral("The created AddressBook could not be materialized."));
                    eraseBook(record.objectId);
                    books.push_back(*parsed.value);
                    acceptedRecords.push_back(&record);
                    continue;
                }
                if (record.kind == AddressBookMutationKind::Update)
                {
                    const auto updated = result.updated.find(record.objectId);
                    if (updated == result.updated.end())
                    {
                        const auto rejected = result.notUpdated.find(record.objectId);
                        if (rejected == result.notUpdated.end())
                            return error(
                                QStringLiteral("The AddressBook/set response omitted an update."));
                        rejectedRecords.push_back(
                            {.record = &record, .errorJson = rejected->second.json});
                        if (record.baseDocument.has_value())
                        {
                            if (const auto restoreError = restoreBook(record, *record.baseDocument))
                                return *restoreError;
                        }
                        else
                            eraseBook(record.objectId);
                        continue;
                    }
                    acceptedRecords.push_back(&record);
                    if (!record.projectedDocument.has_value() || !updated->second.has_value())
                        continue;
                    const auto transformed = javelin::jmap::api::applyPatchObject(
                        *record.projectedDocument, updated->second->json);
                    const auto* document = std::get_if<std::string>(&transformed);
                    if (document == nullptr)
                        return error(
                            QStringLiteral("The AddressBook update transformation is invalid."));
                    const auto parsed =
                        javelin::jmap::api::parseAddressBookDocument(record.objectId, *document);
                    if (!parsed.ok() || !parsed.value.has_value())
                        return error(
                            QStringLiteral("The updated AddressBook could not be materialized."));
                    eraseBook(record.objectId);
                    books.push_back(*parsed.value);
                    continue;
                }
                if (std::ranges::find(result.destroyed, record.objectId) == result.destroyed.end())
                {
                    const auto rejected = result.notDestroyed.find(record.objectId);
                    if (rejected == result.notDestroyed.end())
                        return error(
                            QStringLiteral("The AddressBook/set response omitted a deletion."));
                    rejectedRecords.push_back(
                        {.record = &record, .errorJson = rejected->second.json});
                    if (record.baseDocument.has_value())
                    {
                        if (const auto restoreError = restoreBook(record, *record.baseDocument))
                            return *restoreError;
                    }
                    continue;
                }
                acceptedRecords.push_back(&record);
            }

            auto transactionResult = javelin::jmap::sync::MutationProjectionTransaction::begin(
                connection, QStringLiteral("Reconcile AddressBook mutations"));
            if (const auto* cacheError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
                return error(cacheError->message,
                             javelin::jmap::OperationErrorCode::LocalStorageFailure);
            auto transaction = std::get<javelin::jmap::sync::MutationProjectionTransaction>(
                std::move(transactionResult));
            for (const auto* record : acceptedRecords)
            {
                if (const auto cacheError = transaction.transition(
                        record->mutationId, javelin::jmap::sync::MutationStatus::Accepted,
                        result.newState))
                    return error(cacheError->message,
                                 javelin::jmap::OperationErrorCode::LocalStorageFailure);
            }
            for (const auto& rejected : rejectedRecords)
            {
                if (const auto cacheError = transaction.transition(
                        rejected.record->mutationId, javelin::jmap::sync::MutationStatus::Rejected,
                        std::nullopt, rejected.errorJson))
                    return error(cacheError->message,
                                 javelin::jmap::OperationErrorCode::LocalStorageFailure);
            }
            if (!acceptedRecords.empty())
            {
                const std::array domains{javelin::jmap::sync::ConsistencyDomain{
                    .accountId = result.accountId,
                    .dataType = "AddressBook",
                }};
                if (const auto cacheError = transaction.advance(domains))
                    return error(cacheError->message,
                                 javelin::jmap::OperationErrorCode::LocalStorageFailure);
            }
            if (const auto cacheError = repository.replaceAddressBooks(
                    transaction.cacheTransaction(), result.accountId, books, result.newState))
                return error(cacheError->message,
                             javelin::jmap::OperationErrorCode::LocalStorageFailure);
            for (const auto* record : acceptedRecords)
            {
                if (const auto cacheError = transaction.remove(record->mutationId))
                    return error(cacheError->message,
                                 javelin::jmap::OperationErrorCode::LocalStorageFailure);
            }
            if (const auto cacheError = transaction.commit())
                return error(cacheError->message,
                             javelin::jmap::OperationErrorCode::LocalStorageFailure);
            repository.notifyChanged(result.accountId);
            if (!rejectedRecords.empty())
                return error(QStringLiteral("The server rejected one or more Contacts changes."),
                             javelin::jmap::OperationErrorCode::Conflict);
            return ContactMutationSummary{
                .accountId = result.accountId,
                .newState = result.newState,
                .createdId = createdId(result),
                .createdIds = createdIds(result),
                .receipt = contactReceipt(result, "AddressBook"),
            };
        }

        [[nodiscard]] ContactMutationResult
        reconcileContactMutations(javelin::jmap::cache::DatabaseConnection& connection,
                                  javelin::jmap::cache::ContactRepository& repository,
                                  const std::vector<ContactMutationRecord>& records,
                                  const javelin::jmap::api::SetResult& result)
        {
            struct RejectedMutation
            {
                const ContactMutationRecord* record;
                std::string errorJson;
            };
            std::vector<const ContactMutationRecord*> acceptedRecords;
            std::vector<RejectedMutation> rejectedRecords;
            std::vector<ContactSummary> projectedContacts;
            std::vector<std::string> removedIds;
            for (const auto& record : records)
            {
                if (record.kind == ContactMutationKind::Create)
                {
                    if (!record.creationId.has_value())
                    {
                        return error(
                            QStringLiteral("A ContactCard creation lost its creation id."));
                    }
                    const auto created = result.created.find(*record.creationId);
                    if (created == result.created.end())
                    {
                        const auto rejected = result.notCreated.find(*record.creationId);
                        if (rejected == result.notCreated.end())
                            return error(QStringLiteral(
                                "The ContactCard/set response omitted a created object."));
                        rejectedRecords.push_back(
                            {.record = &record, .errorJson = rejected->second.json});
                        removedIds.push_back(record.objectId);
                        continue;
                    }
                    const auto contactId = createdObjectId(created->second);
                    if (!contactId.has_value())
                    {
                        return error(QStringLiteral(
                            "The ContactCard/set response omitted the created object id."));
                    }
                    const auto& baseDocument =
                        record.projectedDocument.value_or(record.requestedDocument);
                    const auto transformed =
                        javelin::jmap::api::applyPatchObject(baseDocument, created->second.json);
                    const auto* document = std::get_if<std::string>(&transformed);
                    if (document == nullptr)
                    {
                        return error(QStringLiteral(
                            "The ContactCard/set response contained an invalid transformation."));
                    }
                    auto summary =
                        summarizeContact(record.accountId, javelin::jmap::api::ContactCard{
                                                               .id = *contactId,
                                                               .uid = {},
                                                               .kind = {},
                                                               .document = *document,
                                                           });
                    if (!summary.has_value())
                    {
                        return error(QStringLiteral(
                            "The created ContactCard could not be materialized locally."));
                    }
                    acceptedRecords.push_back(&record);
                    projectedContacts.push_back(std::move(*summary));
                    removedIds.push_back(record.objectId);
                    continue;
                }
                if (record.kind == ContactMutationKind::Update)
                {
                    const auto updated = result.updated.find(record.objectId);
                    if (updated == result.updated.end())
                    {
                        const auto rejected = result.notUpdated.find(record.objectId);
                        if (rejected == result.notUpdated.end())
                            return error(QStringLiteral(
                                "The ContactCard/set response omitted an updated object."));
                        rejectedRecords.push_back(
                            {.record = &record, .errorJson = rejected->second.json});
                        if (record.baseDocument.has_value())
                        {
                            auto restored = summarizeContact(record.accountId,
                                                             javelin::jmap::api::ContactCard{
                                                                 .id = record.objectId,
                                                                 .uid = {},
                                                                 .kind = {},
                                                                 .document = *record.baseDocument,
                                                             });
                            if (!restored.has_value())
                                return error(QStringLiteral(
                                    "A rejected ContactCard could not be restored locally."));
                            projectedContacts.push_back(std::move(*restored));
                        }
                        continue;
                    }
                    acceptedRecords.push_back(&record);
                    if (!record.projectedDocument.has_value() || !updated->second.has_value())
                    {
                        continue;
                    }
                    const auto transformed = javelin::jmap::api::applyPatchObject(
                        *record.projectedDocument, updated->second->json);
                    const auto* document = std::get_if<std::string>(&transformed);
                    if (document == nullptr)
                    {
                        return error(QStringLiteral(
                            "The ContactCard/set response contained an invalid transformation."));
                    }
                    auto summary =
                        summarizeContact(record.accountId, javelin::jmap::api::ContactCard{
                                                               .id = record.objectId,
                                                               .uid = {},
                                                               .kind = {},
                                                               .document = *document,
                                                           });
                    if (!summary.has_value())
                    {
                        return error(QStringLiteral(
                            "The updated ContactCard could not be materialized locally."));
                    }
                    projectedContacts.push_back(std::move(*summary));
                    continue;
                }
                if (std::ranges::find(result.destroyed, record.objectId) == result.destroyed.end())
                {
                    const auto rejected = result.notDestroyed.find(record.objectId);
                    if (rejected == result.notDestroyed.end())
                        return error(QStringLiteral(
                            "The ContactCard/set response omitted a destroyed object."));
                    rejectedRecords.push_back(
                        {.record = &record, .errorJson = rejected->second.json});
                    if (record.baseDocument.has_value())
                    {
                        auto restored =
                            summarizeContact(record.accountId, javelin::jmap::api::ContactCard{
                                                                   .id = record.objectId,
                                                                   .uid = {},
                                                                   .kind = {},
                                                                   .document = *record.baseDocument,
                                                               });
                        if (!restored.has_value())
                            return error(QStringLiteral(
                                "A rejected ContactCard could not be restored locally."));
                        projectedContacts.push_back(std::move(*restored));
                    }
                    continue;
                }
                acceptedRecords.push_back(&record);
            }

            auto transactionResult = javelin::jmap::sync::MutationProjectionTransaction::begin(
                connection, QStringLiteral("Reconcile ContactCard mutations"));
            if (const auto* cacheError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            {
                return error(cacheError->message,
                             javelin::jmap::OperationErrorCode::LocalStorageFailure);
            }
            auto transaction = std::get<javelin::jmap::sync::MutationProjectionTransaction>(
                std::move(transactionResult));
            for (const auto* record : acceptedRecords)
            {
                if (const auto cacheError = transaction.transition(
                        record->mutationId, javelin::jmap::sync::MutationStatus::Accepted,
                        result.newState))
                {
                    return error(cacheError->message,
                                 javelin::jmap::OperationErrorCode::LocalStorageFailure);
                }
            }
            for (const auto& rejected : rejectedRecords)
            {
                if (const auto cacheError = transaction.transition(
                        rejected.record->mutationId, javelin::jmap::sync::MutationStatus::Rejected,
                        std::nullopt, rejected.errorJson))
                {
                    return error(cacheError->message,
                                 javelin::jmap::OperationErrorCode::LocalStorageFailure);
                }
            }
            if (!acceptedRecords.empty())
            {
                const std::array domains{javelin::jmap::sync::ConsistencyDomain{
                    .accountId = result.accountId,
                    .dataType = "ContactCard",
                }};
                if (const auto cacheError = transaction.advance(domains))
                    return error(cacheError->message,
                                 javelin::jmap::OperationErrorCode::LocalStorageFailure);
            }
            if (const auto cacheError =
                    repository.projectContacts(transaction.cacheTransaction(), result.accountId,
                                               projectedContacts, removedIds))
            {
                return error(cacheError->message,
                             javelin::jmap::OperationErrorCode::LocalStorageFailure);
            }
            for (const auto* record : acceptedRecords)
            {
                if (const auto cacheError = transaction.remove(record->mutationId))
                {
                    return error(cacheError->message,
                                 javelin::jmap::OperationErrorCode::LocalStorageFailure);
                }
            }
            if (const auto cacheError = transaction.commit())
                return error(cacheError->message,
                             javelin::jmap::OperationErrorCode::LocalStorageFailure);
            repository.notifyChanged(result.accountId);
            if (!rejectedRecords.empty())
            {
                return error(QStringLiteral("The server rejected one or more Contacts changes."),
                             javelin::jmap::OperationErrorCode::Conflict);
            }
            return ContactMutationSummary{
                .accountId = result.accountId,
                .newState = result.newState,
                .createdId = createdId(result),
                .createdIds = createdIds(result),
                .receipt = contactReceipt(result, "ContactCard"),
            };
        }

        [[nodiscard]] ContactMutationResult
        reconcileContactCopy(javelin::jmap::cache::DatabaseConnection& connection,
                             javelin::jmap::cache::ContactRepository& repository,
                             const std::vector<ContactMutationRecord>& records,
                             const CopyObjectsResponse& responseValue)
        {
            struct Transition
            {
                const ContactMutationRecord* record;
                javelin::jmap::sync::MutationStatus status;
                std::optional<std::string> state;
                std::optional<std::string> errorJson;
            };
            std::vector<Transition> transitions;
            std::vector<ContactSummary> destinationContacts;
            std::vector<std::string> destinationRemoved;
            std::vector<ContactSummary> sourceContacts;
            bool hasRejected = false;
            bool hasUnknown = false;

            const auto sourceRecordFor = [&records](const std::optional<std::string>& creationId)
                -> const ContactMutationRecord*
            {
                if (!creationId.has_value())
                    return nullptr;
                const auto found =
                    std::ranges::find_if(records,
                                         [&creationId](const ContactMutationRecord& record)
                                         {
                                             return record.kind == ContactMutationKind::Destroy &&
                                                    record.creationId == creationId;
                                         });
                return found == records.end() ? nullptr : &*found;
            };
            const auto restoreSource = [&sourceContacts](const ContactMutationRecord& record)
                -> std::optional<javelin::jmap::OperationError>
            {
                if (!record.baseDocument.has_value())
                    return std::nullopt;
                auto restored =
                    summarizeContact(record.accountId, javelin::jmap::api::ContactCard{
                                                           .id = record.objectId,
                                                           .uid = {},
                                                           .kind = {},
                                                           .document = *record.baseDocument,
                                                       });
                if (!restored.has_value())
                    return error(QStringLiteral("A copied contact could not be restored locally."),
                                 javelin::jmap::OperationErrorCode::LocalStorageFailure);
                sourceContacts.push_back(std::move(*restored));
                return std::nullopt;
            };

            for (const auto& record : records)
            {
                if (record.kind != ContactMutationKind::Create)
                    continue;
                if (!record.creationId.has_value())
                    return error(QStringLiteral("A ContactCard copy lost its creation id."));
                const auto* sourceRecord = sourceRecordFor(record.creationId);
                const auto created = responseValue.copied.created.find(*record.creationId);
                if (created == responseValue.copied.created.end())
                {
                    const auto rejected = responseValue.copied.notCreated.find(*record.creationId);
                    if (rejected == responseValue.copied.notCreated.end())
                        return error(QStringLiteral(
                                         "The ContactCard/copy response omitted a copied object."),
                                     javelin::jmap::OperationErrorCode::ProtocolViolation);
                    hasRejected = true;
                    destinationRemoved.push_back(record.objectId);
                    transitions.push_back({.record = &record,
                                           .status = javelin::jmap::sync::MutationStatus::Rejected,
                                           .state = std::nullopt,
                                           .errorJson = rejected->second.json});
                    if (sourceRecord != nullptr)
                    {
                        if (const auto restoreError = restoreSource(*sourceRecord))
                            return *restoreError;
                        transitions.push_back(
                            {.record = sourceRecord,
                             .status = javelin::jmap::sync::MutationStatus::Rejected,
                             .state = std::nullopt,
                             .errorJson = rejected->second.json});
                    }
                    continue;
                }

                const auto contactId = createdObjectId(created->second);
                if (!contactId.has_value() || !record.projectedDocument.has_value())
                    return error(
                        QStringLiteral("The ContactCard/copy response omitted the copied id."),
                        javelin::jmap::OperationErrorCode::ProtocolViolation);
                const auto transformed = javelin::jmap::api::applyPatchObject(
                    *record.projectedDocument, created->second.json);
                const auto* document = std::get_if<std::string>(&transformed);
                if (document == nullptr)
                    return error(QStringLiteral("The ContactCard/copy transformation is invalid."),
                                 javelin::jmap::OperationErrorCode::ProtocolViolation);
                auto copied = summarizeContact(record.accountId, javelin::jmap::api::ContactCard{
                                                                     .id = *contactId,
                                                                     .uid = {},
                                                                     .kind = {},
                                                                     .document = *document,
                                                                 });
                if (!copied.has_value())
                    return error(QStringLiteral("The copied ContactCard is invalid."),
                                 javelin::jmap::OperationErrorCode::ProtocolViolation);
                destinationContacts.push_back(std::move(*copied));
                destinationRemoved.push_back(record.objectId);
                transitions.push_back({.record = &record,
                                       .status = javelin::jmap::sync::MutationStatus::Accepted,
                                       .state = responseValue.copied.newState,
                                       .errorJson = std::nullopt});

                if (sourceRecord == nullptr)
                    continue;
                if (!responseValue.destroyedOriginals.has_value())
                {
                    hasUnknown = true;
                    transitions.push_back({.record = sourceRecord,
                                           .status = javelin::jmap::sync::MutationStatus::Unknown,
                                           .state = std::nullopt,
                                           .errorJson = std::nullopt});
                    continue;
                }
                const auto& destroyed = *responseValue.destroyedOriginals;
                if (std::ranges::find(destroyed.destroyed, sourceRecord->objectId) !=
                    destroyed.destroyed.end())
                {
                    transitions.push_back({.record = sourceRecord,
                                           .status = javelin::jmap::sync::MutationStatus::Accepted,
                                           .state = destroyed.newState,
                                           .errorJson = std::nullopt});
                    continue;
                }
                const auto rejected = destroyed.notDestroyed.find(sourceRecord->objectId);
                if (rejected == destroyed.notDestroyed.end())
                {
                    hasUnknown = true;
                    transitions.push_back({.record = sourceRecord,
                                           .status = javelin::jmap::sync::MutationStatus::Unknown,
                                           .state = std::nullopt,
                                           .errorJson = std::nullopt});
                    continue;
                }
                hasRejected = true;
                if (const auto restoreError = restoreSource(*sourceRecord))
                    return *restoreError;
                transitions.push_back({.record = sourceRecord,
                                       .status = javelin::jmap::sync::MutationStatus::Rejected,
                                       .state = std::nullopt,
                                       .errorJson = rejected->second.json});
            }

            auto transactionResult = javelin::jmap::sync::MutationProjectionTransaction::begin(
                connection, QStringLiteral("Reconcile ContactCard copy"));
            if (const auto* cacheError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
                return error(cacheError->message,
                             javelin::jmap::OperationErrorCode::LocalStorageFailure);
            auto transaction = std::get<javelin::jmap::sync::MutationProjectionTransaction>(
                std::move(transactionResult));
            std::vector<javelin::jmap::sync::ConsistencyDomain> acceptedDomains;
            for (const auto& transition : transitions)
            {
                if (const auto cacheError =
                        transaction.transition(transition.record->mutationId, transition.status,
                                               transition.state, transition.errorJson))
                    return error(cacheError->message,
                                 javelin::jmap::OperationErrorCode::LocalStorageFailure);
                if (transition.status == javelin::jmap::sync::MutationStatus::Accepted)
                {
                    const javelin::jmap::sync::ConsistencyDomain domain{
                        .accountId = transition.record->accountId,
                        .dataType = "ContactCard",
                    };
                    if (std::ranges::find(acceptedDomains, domain) == acceptedDomains.end())
                        acceptedDomains.push_back(domain);
                }
            }
            if (const auto cacheError = transaction.advance(acceptedDomains))
                return error(cacheError->message,
                             javelin::jmap::OperationErrorCode::LocalStorageFailure);
            if (const auto cacheError = repository.projectContacts(
                    transaction.cacheTransaction(), responseValue.copied.accountId,
                    destinationContacts, destinationRemoved))
                return error(cacheError->message,
                             javelin::jmap::OperationErrorCode::LocalStorageFailure);
            if (!sourceContacts.empty())
            {
                const auto& sourceAccountId = sourceContacts.front().accountId;
                if (const auto cacheError = repository.projectContacts(
                        transaction.cacheTransaction(), sourceAccountId, sourceContacts, {}))
                    return error(cacheError->message,
                                 javelin::jmap::OperationErrorCode::LocalStorageFailure);
            }
            for (const auto& transition : transitions)
            {
                if (transition.status == javelin::jmap::sync::MutationStatus::Accepted)
                {
                    if (const auto cacheError = transaction.remove(transition.record->mutationId))
                        return error(cacheError->message,
                                     javelin::jmap::OperationErrorCode::LocalStorageFailure);
                }
            }
            if (const auto cacheError = transaction.commit())
                return error(cacheError->message,
                             javelin::jmap::OperationErrorCode::LocalStorageFailure);
            for (const auto& accountId : {records.front().accountId, records.back().accountId})
                repository.notifyChanged(accountId);
            if (hasUnknown)
                return error(
                    QStringLiteral("The contact was copied, but source removal is unconfirmed."),
                    javelin::jmap::OperationErrorCode::ProtocolViolation);
            if (hasRejected)
                return error(QStringLiteral("The server rejected part of the contact copy."),
                             javelin::jmap::OperationErrorCode::Conflict);
            auto receipt = contactReceipt(responseValue.copied, "ContactCard");
            if (responseValue.destroyedOriginals.has_value())
            {
                auto sourceReceipt =
                    contactReceipt(*responseValue.destroyedOriginals, "ContactCard");
                receipt.domains.insert(receipt.domains.end(),
                                       std::make_move_iterator(sourceReceipt.domains.begin()),
                                       std::make_move_iterator(sourceReceipt.domains.end()));
                receipt.acceptedObjectIds.insert(
                    receipt.acceptedObjectIds.end(),
                    std::make_move_iterator(sourceReceipt.acceptedObjectIds.begin()),
                    std::make_move_iterator(sourceReceipt.acceptedObjectIds.end()));
                receipt.rejectedObjectIds.insert(
                    receipt.rejectedObjectIds.end(),
                    std::make_move_iterator(sourceReceipt.rejectedObjectIds.begin()),
                    std::make_move_iterator(sourceReceipt.rejectedObjectIds.end()));
            }
            return ContactMutationSummary{
                .accountId = responseValue.copied.accountId,
                .newState = responseValue.copied.newState,
                .createdId = createdId(responseValue.copied),
                .createdIds = createdIds(responseValue.copied),
                .receipt = std::move(receipt),
            };
        }

        [[nodiscard]] std::optional<std::string>
        canonicalContactDocument(const std::string_view document, const bool removeId)
        {
            std::string buffer{document};
            glz::generic value;
            if (glz::read_json(value, buffer) || !value.is_object())
                return std::nullopt;
            if (removeId)
                value.get_object().erase("id");
            std::string canonical;
            if (glz::write_json(value, canonical))
                return std::nullopt;
            return canonical;
        }

        struct RebasedContacts
        {
            std::vector<javelin::jmap::api::AddressBook> books;
            std::vector<ContactSummary> contacts;
            std::vector<const AddressBookMutationRecord*> acceptedBooks;
            std::vector<const AddressBookMutationRecord*> rejectedBooks;
            std::vector<const ContactMutationRecord*> acceptedContacts;
            std::vector<const ContactMutationRecord*> rejectedContacts;
        };

        [[nodiscard]] std::variant<RebasedContacts, javelin::jmap::OperationError>
        rebaseContacts(const std::string& accountId,
                       std::vector<javelin::jmap::api::AddressBook> serverBooks,
                       std::vector<ContactSummary> serverContacts,
                       const std::vector<AddressBookMutationRecord>& bookMutations,
                       const std::vector<ContactMutationRecord>& contactMutations)
        {
            RebasedContacts result{
                .books = std::move(serverBooks),
                .contacts = std::move(serverContacts),
                .acceptedBooks = {},
                .rejectedBooks = {},
                .acceptedContacts = {},
                .rejectedContacts = {},
            };
            const auto serverBooksSnapshot = result.books;
            const auto serverContactsSnapshot = result.contacts;
            for (const auto& mutation : bookMutations)
            {
                const auto server = std::ranges::find(serverBooksSnapshot, mutation.objectId,
                                                      &javelin::jmap::api::AddressBook::id);
                if (mutation.kind == AddressBookMutationKind::Destroy)
                {
                    if (mutation.status == javelin::jmap::sync::MutationStatus::Unknown &&
                        server == serverBooksSnapshot.end())
                        result.acceptedBooks.push_back(&mutation);
                    std::erase_if(result.books, [&mutation](const auto& book)
                                  { return book.id == mutation.objectId; });
                    continue;
                }
                if (!mutation.projectedDocument.has_value())
                    continue;
                if (mutation.kind == AddressBookMutationKind::Update &&
                    mutation.status == javelin::jmap::sync::MutationStatus::Unknown &&
                    server == serverBooksSnapshot.end())
                {
                    result.rejectedBooks.push_back(&mutation);
                    continue;
                }
                const auto projected = javelin::jmap::api::parseAddressBookDocument(
                    mutation.objectId, *mutation.projectedDocument);
                if (!projected.ok() || !projected.value.has_value())
                    return error(QStringLiteral("A projected AddressBook is invalid."),
                                 javelin::jmap::OperationErrorCode::LocalStorageFailure);
                auto visible = *projected.value;
                bool confirmed = false;
                if (mutation.kind == AddressBookMutationKind::Create)
                {
                    const auto matched = std::ranges::find_if(serverBooksSnapshot,
                                                              [&visible](auto candidate)
                                                              {
                                                                  candidate.id = visible.id;
                                                                  return candidate == visible;
                                                              });
                    if (matched != serverBooksSnapshot.end())
                    {
                        visible = *matched;
                        confirmed = true;
                    }
                }
                else if (server != serverBooksSnapshot.end())
                    confirmed = *server == visible;
                if (mutation.status == javelin::jmap::sync::MutationStatus::Unknown && confirmed)
                    result.acceptedBooks.push_back(&mutation);
                std::erase_if(result.books, [&mutation](const auto& book)
                              { return book.id == mutation.objectId; });
                result.books.push_back(std::move(visible));
            }
            for (const auto& mutation : contactMutations)
            {
                const auto server = std::ranges::find(serverContactsSnapshot, mutation.objectId,
                                                      &ContactSummary::id);
                if (mutation.kind == ContactMutationKind::Destroy)
                {
                    if (mutation.status == javelin::jmap::sync::MutationStatus::Unknown &&
                        server == serverContactsSnapshot.end())
                        result.acceptedContacts.push_back(&mutation);
                    std::erase_if(result.contacts, [&mutation](const ContactSummary& contact)
                                  { return contact.id == mutation.objectId; });
                    continue;
                }
                if (!mutation.projectedDocument.has_value())
                    continue;
                if (mutation.kind == ContactMutationKind::Update &&
                    mutation.status == javelin::jmap::sync::MutationStatus::Unknown &&
                    server == serverContactsSnapshot.end())
                {
                    result.rejectedContacts.push_back(&mutation);
                    continue;
                }
                auto projected =
                    summarizeContact(accountId, javelin::jmap::api::ContactCard{
                                                    .id = mutation.objectId,
                                                    .uid = {},
                                                    .kind = {},
                                                    .document = *mutation.projectedDocument,
                                                });
                if (!projected.has_value())
                    return error(QStringLiteral("A projected ContactCard is invalid."),
                                 javelin::jmap::OperationErrorCode::LocalStorageFailure);
                bool confirmed = false;
                if (mutation.kind == ContactMutationKind::Create)
                {
                    const auto matched = std::ranges::find(serverContactsSnapshot, projected->uid,
                                                           &ContactSummary::uid);
                    if (matched != serverContactsSnapshot.end())
                    {
                        // Card UIDs are supplied by the client and immutable. They safely
                        // correlate a create whose response was lost even when the server
                        // canonicalizes or adds Card properties.
                        projected = *matched;
                        confirmed = true;
                    }
                }
                else if (server != serverContactsSnapshot.end())
                {
                    const auto expected = canonicalContactDocument(projected->document, false);
                    const auto actual = canonicalContactDocument(server->document, false);
                    confirmed = expected.has_value() && expected == actual;
                }
                if (mutation.status == javelin::jmap::sync::MutationStatus::Unknown && confirmed)
                    result.acceptedContacts.push_back(&mutation);
                std::erase_if(result.contacts, [&mutation](const ContactSummary& contact)
                              { return contact.id == mutation.objectId; });
                if (mutation.kind == ContactMutationKind::Create)
                    std::erase_if(result.contacts, [&projected](const ContactSummary& contact)
                                  { return contact.uid == projected->uid; });
                result.contacts.push_back(std::move(*projected));
            }
            return result;
        }
    } // namespace

    ContactService::ContactService(javelin::jmap::cache::DatabaseConnection& connection,
                                   javelin::jmap::cache::ContactRepository& repository,
                                   javelin::jmap::api::AbstractTransport& resourceTransport,
                                   javelin::jmap::api::JmapMethodTransport& methodTransport)
        : m_connection(connection), m_repository(repository),
          m_resourceTransport(resourceTransport), m_methodTransport(methodTransport)
    {
    }

    QCoro::Task<ContactRefreshResult>
    ContactService::refreshAll(javelin::jmap::LiveConnectionSettings settings,
                               std::string ownerAccountId)
    {
        const auto sessionResult = loadSession(m_connection, ownerAccountId);
        if (const auto* loadError = std::get_if<javelin::jmap::OperationError>(&sessionResult))
        {
            co_return *loadError;
        }
        const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);
        ContactRefreshSummary summary;
        for (const auto& [accountId, account] : session.accounts)
        {
            if (!account.accountCapabilities.contacts.has_value())
            {
                continue;
            }
            AddressBookMutationJournal addressBookJournal{m_connection, m_repository};
            ContactMutationJournal contactJournal{m_connection, m_repository};
            const auto activeBooksResult = addressBookJournal.listActive(accountId);
            const auto activeContactsResult = contactJournal.listActive(accountId);
            if (const auto* cacheError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&activeBooksResult))
                co_return error(cacheError->message);
            if (const auto* cacheError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&activeContactsResult))
                co_return error(cacheError->message);
            const auto& activeBooks =
                std::get<std::vector<AddressBookMutationRecord>>(activeBooksResult);
            const auto& activeContacts =
                std::get<std::vector<ContactMutationRecord>>(activeContactsResult);
            javelin::jmap::cache::SyncStateRepository states{m_connection};
            const auto cachedState =
                states.find({.accountId = accountId, .objectType = "ContactCard", .queryKey = {}});
            if (const auto* stateError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&cachedState))
                co_return error(stateError->message);
            const auto& stateRecord =
                std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(cachedState);
            if (activeBooks.empty() && activeContacts.empty() && stateRecord.has_value() &&
                !stateRecord->stateToken.empty())
            {
                auto incremental = co_await refreshAccountIncrementally(
                    m_repository, m_connection, m_methodTransport, settings, session, accountId,
                    stateRecord->stateToken);
                if (const auto* incrementalError =
                        std::get_if<javelin::jmap::OperationError>(&incremental))
                    co_return *incrementalError;
                if (const auto* refreshed = std::get_if<AccountRefreshSummary>(&incremental))
                {
                    ++summary.accountCount;
                    summary.addressBookCount += refreshed->addressBookCount;
                    summary.contactCount += refreshed->contactCount;
                    continue;
                }
                if (std::holds_alternative<SupersededRefresh>(incremental))
                    continue;
            }
            const auto addressBookFenceResult =
                captureFence(m_connection, accountId, "AddressBook");
            const auto contactFenceResult = captureFence(m_connection, accountId, "ContactCard");
            if (const auto* serviceError =
                    std::get_if<javelin::jmap::OperationError>(&addressBookFenceResult))
                co_return *serviceError;
            if (const auto* serviceError =
                    std::get_if<javelin::jmap::OperationError>(&contactFenceResult))
                co_return *serviceError;
            const auto addressBookFence =
                std::get<javelin::jmap::sync::RefreshFence>(addressBookFenceResult);
            const auto contactFence =
                std::get<javelin::jmap::sync::RefreshFence>(contactFenceResult);
            const auto getArguments =
                javelin::jmap::api::serializeGetRequest({.accountId = accountId,
                                                         .ids = std::nullopt,
                                                         .idsReference = std::nullopt,
                                                         .properties = std::nullopt});
            if (!getArguments.has_value())
            {
                co_return error(QStringLiteral("Unable to serialize Contacts refresh."));
            }
            javelin::jmap::api::RequestBuilder builder;
            builder.useCore().useCapability(std::string{javelin::jmap::api::contactsCapabilityUri});
            static_cast<void>(builder.call(
                javelin::jmap::api::MethodRequest<javelin::jmap::api::AddressBookGetResponse>{
                    .name = "AddressBook/get", .arguments = *getArguments},
                "address-books"));
            static_cast<void>(builder.call(
                javelin::jmap::api::MethodRequest<javelin::jmap::api::ContactCardGetResponse>{
                    .name = "ContactCard/get", .arguments = *getArguments},
                "contact-cards"));
            javelin::jmap::api::MethodCaller caller{m_methodTransport};
            const auto callResult =
                co_await caller.call(context(settings, session, accountId), builder);
            const auto* envelope = std::get_if<javelin::jmap::api::ResponseEnvelope>(&callResult);
            if (envelope == nullptr)
            {
                co_return callError(callResult);
            }
            const auto booksMethod = response(*envelope, "address-books", "AddressBook/get");
            const auto cardsMethod = response(*envelope, "contact-cards", "ContactCard/get");
            if (!booksMethod.has_value() || !cardsMethod.has_value())
            {
                co_return error(QStringLiteral("Contacts refresh response was incomplete."));
            }
            const auto books =
                javelin::jmap::api::parseAddressBookGetResponse(booksMethod->arguments);
            const auto cards =
                javelin::jmap::api::parseContactCardGetResponse(cardsMethod->arguments);
            if (!books.ok() || !cards.ok())
            {
                co_return error(QStringLiteral("Unable to parse the Contacts refresh response."));
            }
            std::vector<ContactSummary> contacts;
            contacts.reserve(cards.value->list.size());
            for (const auto& card : cards.value->list)
            {
                auto contact = summarizeContact(accountId, card);
                if (!contact.has_value())
                {
                    co_return error(QStringLiteral("The server returned an invalid ContactCard."));
                }
                contacts.push_back(std::move(*contact));
            }
            const auto addressBooksCurrent =
                fenceGenerationIsCurrent(m_connection, addressBookFence);
            const auto contactsCurrent = fenceGenerationIsCurrent(m_connection, contactFence);
            if (const auto* serviceError =
                    std::get_if<javelin::jmap::OperationError>(&addressBooksCurrent))
                co_return *serviceError;
            if (const auto* serviceError =
                    std::get_if<javelin::jmap::OperationError>(&contactsCurrent))
                co_return *serviceError;
            if (!std::get<bool>(addressBooksCurrent) || !std::get<bool>(contactsCurrent))
                continue;
            const auto rebasedResult = rebaseContacts(
                accountId, books.value->list, std::move(contacts), activeBooks, activeContacts);
            if (const auto* operationError =
                    std::get_if<javelin::jmap::OperationError>(&rebasedResult))
                co_return *operationError;
            auto rebased = std::get<RebasedContacts>(rebasedResult);
            auto transactionResult = javelin::jmap::sync::MutationProjectionTransaction::begin(
                m_connection, QStringLiteral("Rebase Contacts refresh"));
            if (const auto* cacheError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
                co_return error(cacheError->message);
            auto transaction = std::get<javelin::jmap::sync::MutationProjectionTransaction>(
                std::move(transactionResult));
            std::vector<javelin::jmap::sync::ConsistencyDomain> acceptedDomains;
            if (!rebased.acceptedBooks.empty())
                acceptedDomains.push_back({.accountId = accountId, .dataType = "AddressBook"});
            if (!rebased.acceptedContacts.empty())
                acceptedDomains.push_back({.accountId = accountId, .dataType = "ContactCard"});
            if (const auto cacheError = transaction.advance(acceptedDomains))
                co_return error(cacheError->message);
            for (const auto* mutation : rebased.acceptedBooks)
            {
                if (const auto cacheError = transaction.transition(
                        mutation->mutationId, javelin::jmap::sync::MutationStatus::Accepted,
                        books.value->state))
                    co_return error(cacheError->message);
                if (const auto cacheError = transaction.remove(mutation->mutationId))
                    co_return error(cacheError->message);
            }
            for (const auto* mutation : rebased.acceptedContacts)
            {
                if (const auto cacheError = transaction.transition(
                        mutation->mutationId, javelin::jmap::sync::MutationStatus::Accepted,
                        cards.value->state))
                    co_return error(cacheError->message);
                if (const auto cacheError = transaction.remove(mutation->mutationId))
                    co_return error(cacheError->message);
            }
            constexpr std::string_view notFoundError = R"({"type":"notFound"})";
            for (const auto* mutation : rebased.rejectedBooks)
                if (const auto cacheError = transaction.transition(
                        mutation->mutationId, javelin::jmap::sync::MutationStatus::Rejected,
                        std::nullopt, notFoundError))
                    co_return error(cacheError->message);
            for (const auto* mutation : rebased.rejectedContacts)
                if (const auto cacheError = transaction.transition(
                        mutation->mutationId, javelin::jmap::sync::MutationStatus::Rejected,
                        std::nullopt, notFoundError))
                    co_return error(cacheError->message);
            if (const auto cacheError = m_repository.replaceAll(
                    transaction.cacheTransaction(), accountId, rebased.books, rebased.contacts,
                    books.value->state, cards.value->state))
                co_return error(cacheError->message);
            if (const auto cacheError = transaction.commit())
                co_return error(cacheError->message);
            m_repository.notifyChanged(accountId);
            ++summary.accountCount;
            summary.addressBookCount += rebased.books.size();
            summary.contactCount += rebased.contacts.size();
        }
        co_return summary;
    }

    QCoro::Task<ContactMutationResult>
    ContactService::setAddressBooks(javelin::jmap::LiveConnectionSettings settings,
                                    std::string ownerAccountId,
                                    javelin::jmap::api::AddressBookSetRequest request)
    {
        const auto accountId = request.accountId;
        if (!request.ifInState.has_value())
        {
            const auto state = m_repository.addressBookState(accountId);
            if (const auto* cacheError = std::get_if<javelin::jmap::cache::DatabaseError>(&state))
                co_return error(cacheError->message,
                                javelin::jmap::OperationErrorCode::LocalStorageFailure);
            request.ifInState = std::get<std::optional<std::string>>(state);
        }
        const auto serialized = javelin::jmap::api::serializeAddressBookSetRequest(request);
        if (!serialized.has_value())
            co_return error(QStringLiteral("Unable to serialize the Contacts change."));
        const auto sessionResult = loadSession(m_connection, ownerAccountId);
        if (const auto* loadError = std::get_if<javelin::jmap::OperationError>(&sessionResult))
            co_return *loadError;
        const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);
        const auto account = session.accounts.find(accountId);
        if (account == session.accounts.end() ||
            !account->second.accountCapabilities.contacts.has_value())
            co_return error(QStringLiteral("This account does not support JMAP Contacts."),
                            javelin::jmap::OperationErrorCode::UnsupportedCapability);

        const auto preparedResult = prepareAddressBookMutations(m_repository, request);
        if (const auto* operationError =
                std::get_if<javelin::jmap::OperationError>(&preparedResult))
            co_return *operationError;
        auto prepared = std::get<PreparedAddressBookMutations>(preparedResult);
        AddressBookMutationJournal journal{m_connection, m_repository};
        if (!prepared.records.empty())
        {
            if (const auto cacheError = journal.queue(prepared.records, prepared.projectedBooks,
                                                      request.ifInState.value_or(std::string{})))
                co_return error(cacheError->message,
                                javelin::jmap::OperationErrorCode::LocalStorageFailure);
            if (const auto cacheError = journal.transition(
                    prepared.records, javelin::jmap::sync::MutationStatus::InFlight))
                co_return error(cacheError->message,
                                javelin::jmap::OperationErrorCode::LocalStorageFailure);
        }

        const auto result = co_await setObjects(m_methodTransport, m_connection,
                                                std::move(settings), std::move(ownerAccountId),
                                                accountId, "AddressBook/set", serialized);
        if (const auto* operationError = std::get_if<javelin::jmap::OperationError>(&result))
        {
            if (const auto cacheError = journal.transition(
                    prepared.records, javelin::jmap::sync::MutationStatus::Unknown))
                co_return error(cacheError->message,
                                javelin::jmap::OperationErrorCode::LocalStorageFailure);
            co_return *operationError;
        }
        if (prepared.records.empty())
        {
            const std::array domains{javelin::jmap::sync::ConsistencyDomain{
                .accountId = accountId,
                .dataType = "AddressBook",
            }};
            co_return commitSetResult(m_connection, std::get<javelin::jmap::api::SetResult>(result),
                                      domains);
        }
        auto reconciled =
            reconcileAddressBookMutations(m_connection, m_repository, prepared.records,
                                          std::get<javelin::jmap::api::SetResult>(result));
        if (const auto* reconciliationError =
                std::get_if<javelin::jmap::OperationError>(&reconciled);
            reconciliationError != nullptr &&
            reconciliationError->code != javelin::jmap::OperationErrorCode::Conflict)
            static_cast<void>(
                journal.transition(prepared.records, javelin::jmap::sync::MutationStatus::Unknown));
        co_return reconciled;
    }

    QCoro::Task<ContactMutationResult>
    ContactService::setContactCards(javelin::jmap::LiveConnectionSettings settings,
                                    std::string ownerAccountId,
                                    javelin::jmap::api::ContactCardSetRequest request)
    {
        const auto accountId = request.accountId;
        const auto serialized = javelin::jmap::api::serializeContactCardSetRequest(request);
        if (!serialized.has_value())
        {
            co_return error(QStringLiteral("Unable to serialize the Contacts change."));
        }
        const auto sessionResult = loadSession(m_connection, ownerAccountId);
        if (const auto* loadError = std::get_if<javelin::jmap::OperationError>(&sessionResult))
        {
            co_return *loadError;
        }
        const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);
        const auto account = session.accounts.find(accountId);
        if (account == session.accounts.end() ||
            !account->second.accountCapabilities.contacts.has_value())
        {
            co_return error(QStringLiteral("This account does not support JMAP Contacts."),
                            javelin::jmap::OperationErrorCode::UnsupportedCapability);
        }

        const auto preparedResult = prepareContactMutations(m_repository, request);
        if (const auto* operationError =
                std::get_if<javelin::jmap::OperationError>(&preparedResult))
        {
            co_return *operationError;
        }
        auto prepared = std::get<PreparedContactMutations>(preparedResult);
        ContactMutationJournal journal{m_connection, m_repository};
        if (!prepared.records.empty())
        {
            if (const auto cacheError = journal.queue(prepared.records, prepared.projectedContacts,
                                                      prepared.destroyedIds))
            {
                co_return error(cacheError->message,
                                javelin::jmap::OperationErrorCode::LocalStorageFailure);
            }
            if (const auto cacheError = journal.transition(
                    prepared.records, javelin::jmap::sync::MutationStatus::InFlight))
            {
                co_return error(cacheError->message,
                                javelin::jmap::OperationErrorCode::LocalStorageFailure);
            }
        }

        const auto result = co_await setObjects(m_methodTransport, m_connection,
                                                std::move(settings), std::move(ownerAccountId),
                                                accountId, "ContactCard/set", serialized);
        if (const auto* operationError = std::get_if<javelin::jmap::OperationError>(&result))
        {
            if (!prepared.records.empty())
            {
                if (operationError->code == javelin::jmap::OperationErrorCode::Conflict)
                {
                    const auto errorJson = operationError->message.toStdString();
                    if (const auto cacheError =
                            journal.restoreRejected(prepared.records, errorJson))
                    {
                        co_return error(cacheError->message,
                                        javelin::jmap::OperationErrorCode::LocalStorageFailure);
                    }
                }
                else if (const auto cacheError = journal.transition(
                             prepared.records, javelin::jmap::sync::MutationStatus::Unknown))
                {
                    co_return error(cacheError->message,
                                    javelin::jmap::OperationErrorCode::LocalStorageFailure);
                }
            }
            co_return *operationError;
        }
        if (prepared.records.empty())
        {
            const std::array domains{javelin::jmap::sync::ConsistencyDomain{
                .accountId = accountId,
                .dataType = "ContactCard",
            }};
            co_return commitSetResult(m_connection, std::get<javelin::jmap::api::SetResult>(result),
                                      domains);
        }
        auto accepted = reconcileContactMutations(m_connection, m_repository, prepared.records,
                                                  std::get<javelin::jmap::api::SetResult>(result));
        if (const auto* reconciliationError = std::get_if<javelin::jmap::OperationError>(&accepted);
            reconciliationError != nullptr &&
            reconciliationError->code != javelin::jmap::OperationErrorCode::Conflict)
        {
            static_cast<void>(
                journal.transition(prepared.records, javelin::jmap::sync::MutationStatus::Unknown));
        }
        co_return accepted;
    }

    PreparedContactCardMutation
    ContactService::prepareCreateGroup(CreateContactGroupCommand command) const
    {
        const auto accounts = m_repository.listAccounts();
        if (const auto* cacheError = std::get_if<javelin::jmap::cache::DatabaseError>(&accounts))
            return error(cacheError->message,
                         javelin::jmap::OperationErrorCode::LocalStorageFailure);
        const auto& availableAccounts =
            std::get<std::vector<javelin::jmap::cache::ContactAccount>>(accounts);
        const auto account = std::ranges::find(availableAccounts, command.accountId,
                                               &javelin::jmap::cache::ContactAccount::accountId);
        if (account == availableAccounts.end())
            return error(QStringLiteral("The Contacts account is unavailable."),
                         javelin::jmap::OperationErrorCode::NotFound);
        const auto books = m_repository.listAddressBooks(command.accountId);
        if (const auto* cacheError = std::get_if<javelin::jmap::cache::DatabaseError>(&books))
            return error(cacheError->message,
                         javelin::jmap::OperationErrorCode::LocalStorageFailure);
        const auto& addressBooks = std::get<std::vector<javelin::jmap::api::AddressBook>>(books);
        const auto addressBook = std::ranges::find(addressBooks, command.addressBookId,
                                                   &javelin::jmap::api::AddressBook::id);
        if (account->isReadOnly || addressBook == addressBooks.end() ||
            !addressBook->myRights.mayWrite)
            return error(QStringLiteral("You do not have permission to create this group."),
                         javelin::jmap::OperationErrorCode::PermissionDenied);
        const auto uid = newMutationId();
        const auto document = createContactGroupDocument(std::move(command.name), uid,
                                                         std::move(command.addressBookId));
        if (const auto* message = std::get_if<std::string_view>(&document))
            return error(
                QString::fromUtf8(message->data(), static_cast<qsizetype>(message->size())),
                javelin::jmap::OperationErrorCode::InvalidUserInput);
        const auto state = contactState(m_connection, command.accountId);
        if (const auto* operationError = std::get_if<javelin::jmap::OperationError>(&state))
            return *operationError;
        javelin::jmap::api::ContactCardSetRequest request{
            .accountId = std::move(command.accountId),
            .ifInState = std::get<std::optional<std::string>>(state),
            .create = {},
            .update = {},
            .destroy = {},
        };
        request.create.emplace("new-group-" + uid, javelin::jmap::api::ContactDocument{
                                                       .json = std::get<std::string>(document)});
        return request;
    }

    PreparedContactCardMutation
    ContactService::prepareGroupMembership(SetContactGroupMembershipCommand command) const
    {
        if (command.memberUids.empty() ||
            std::ranges::any_of(command.memberUids,
                                [](const auto& memberUid) { return memberUid.empty(); }))
            return error(QStringLiteral("A contact group member requires a uid."),
                         javelin::jmap::OperationErrorCode::InvalidUserInput);
        const auto cached = m_repository.findContact(command.accountId, command.groupId);
        if (const auto* cacheError = std::get_if<javelin::jmap::cache::DatabaseError>(&cached))
            return error(cacheError->message,
                         javelin::jmap::OperationErrorCode::LocalStorageFailure);
        const auto& group =
            std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(cached);
        if (!group.has_value() || group->kind != "group")
            return error(QStringLiteral("The selected contact group is unavailable."),
                         javelin::jmap::OperationErrorCode::NotFound);
        const auto accounts = m_repository.listAccounts();
        if (const auto* cacheError = std::get_if<javelin::jmap::cache::DatabaseError>(&accounts))
            return error(cacheError->message,
                         javelin::jmap::OperationErrorCode::LocalStorageFailure);
        const auto& availableAccounts =
            std::get<std::vector<javelin::jmap::cache::ContactAccount>>(accounts);
        const auto account = std::ranges::find(availableAccounts, command.accountId,
                                               &javelin::jmap::cache::ContactAccount::accountId);
        const auto books = m_repository.listAddressBooks(command.accountId);
        if (const auto* cacheError = std::get_if<javelin::jmap::cache::DatabaseError>(&books))
            return error(cacheError->message,
                         javelin::jmap::OperationErrorCode::LocalStorageFailure);
        const auto& addressBooks = std::get<std::vector<javelin::jmap::api::AddressBook>>(books);
        if (account == availableAccounts.end() ||
            !contactActionRights(account->isReadOnly, addressBooks, group->addressBookIds)
                 .mayModify)
            return error(QStringLiteral("You do not have permission to change this group."),
                         javelin::jmap::OperationErrorCode::PermissionDenied);
        const auto editor = contactEditorData(group->document);
        const auto* data = std::get_if<ContactEditorData>(&editor);
        if (data == nullptr)
            return error(QStringLiteral("The contact group document is invalid."),
                         javelin::jmap::OperationErrorCode::ProtocolViolation);
        std::vector<std::string> changedMembers;
        changedMembers.reserve(command.memberUids.size());
        for (auto& memberUid : command.memberUids)
        {
            const bool alreadyIncluded =
                std::ranges::find(data->members, memberUid) != data->members.end();
            if (alreadyIncluded != command.included)
                changedMembers.push_back(std::move(memberUid));
        }
        const auto state = contactState(m_connection, command.accountId);
        if (const auto* operationError = std::get_if<javelin::jmap::OperationError>(&state))
            return *operationError;
        const auto stateToken = std::get<std::optional<std::string>>(state);
        if (changedMembers.empty())
            return javelin::jmap::api::ContactCardSetRequest{
                .accountId = std::move(command.accountId),
                .ifInState = stateToken,
                .create = {},
                .update = {},
                .destroy = {},
            };
        const auto patch = contactGroupMembershipPatch(std::span<const std::string>{changedMembers},
                                                       command.included);
        if (const auto* message = std::get_if<std::string_view>(&patch))
            return error(
                QString::fromUtf8(message->data(), static_cast<qsizetype>(message->size())),
                javelin::jmap::OperationErrorCode::InvalidUserInput);
        javelin::jmap::api::ContactCardSetRequest request{
            .accountId = std::move(command.accountId),
            .ifInState = stateToken,
            .create = {},
            .update = {},
            .destroy = {},
        };
        request.update.emplace(
            std::move(command.groupId),
            javelin::jmap::api::ContactDocument{.json = std::get<std::string>(patch)});
        return request;
    }

    QCoro::Task<ContactMutationResult>
    ContactService::createGroup(javelin::jmap::LiveConnectionSettings settings,
                                std::string ownerAccountId, CreateContactGroupCommand command)
    {
        auto prepared = prepareCreateGroup(std::move(command));
        if (const auto* operationError = std::get_if<javelin::jmap::OperationError>(&prepared))
            co_return *operationError;
        co_return co_await setContactCards(
            std::move(settings), std::move(ownerAccountId),
            std::get<javelin::jmap::api::ContactCardSetRequest>(std::move(prepared)));
    }

    QCoro::Task<ContactMutationResult>
    ContactService::setGroupMembership(javelin::jmap::LiveConnectionSettings settings,
                                       std::string ownerAccountId,
                                       SetContactGroupMembershipCommand command)
    {
        auto prepared = prepareGroupMembership(std::move(command));
        if (const auto* operationError = std::get_if<javelin::jmap::OperationError>(&prepared))
            co_return *operationError;
        auto request = std::get<javelin::jmap::api::ContactCardSetRequest>(std::move(prepared));
        if (request.update.empty())
            co_return ContactMutationSummary{
                .accountId = std::move(request.accountId),
                .newState = request.ifInState.value_or(std::string{}),
                .createdId = std::nullopt,
                .createdIds = {},
                .receipt = {},
            };
        co_return co_await setContactCards(std::move(settings), std::move(ownerAccountId),
                                           std::move(request));
    }

    QCoro::Task<ContactMutationResult>
    ContactService::copyContactCards(javelin::jmap::LiveConnectionSettings settings,
                                     std::string ownerAccountId,
                                     javelin::jmap::api::ContactCardCopyRequest request)
    {
        if (!request.ifFromInState.has_value())
        {
            const auto state = contactState(m_connection, request.fromAccountId);
            if (const auto* operationError = std::get_if<javelin::jmap::OperationError>(&state))
                co_return *operationError;
            request.ifFromInState = std::get<std::optional<std::string>>(state);
        }
        if (!request.ifInState.has_value())
        {
            const auto state = contactState(m_connection, request.accountId);
            if (const auto* operationError = std::get_if<javelin::jmap::OperationError>(&state))
                co_return *operationError;
            request.ifInState = std::get<std::optional<std::string>>(state);
        }
        if (request.onSuccessDestroyOriginal && !request.destroyFromIfInState.has_value())
            request.destroyFromIfInState = request.ifFromInState;
        const auto serialized = javelin::jmap::api::serializeContactCardCopyRequest(request);
        if (!serialized.has_value())
            co_return error(QStringLiteral("Unable to serialize the Contacts copy."));

        const auto preparedResult = prepareContactCopy(m_repository, request);
        if (const auto* operationError =
                std::get_if<javelin::jmap::OperationError>(&preparedResult))
            co_return *operationError;
        auto prepared = std::get<PreparedContactCopy>(preparedResult);
        ContactMutationJournal journal{m_connection, m_repository};
        if (!prepared.records.empty())
        {
            if (const auto cacheError = journal.queueGroup(prepared.records, prepared.projections))
                co_return error(cacheError->message,
                                javelin::jmap::OperationErrorCode::LocalStorageFailure);
            if (const auto cacheError = journal.transition(
                    prepared.records, javelin::jmap::sync::MutationStatus::InFlight))
                co_return error(cacheError->message,
                                javelin::jmap::OperationErrorCode::LocalStorageFailure);
        }

        const auto result =
            co_await copyObjects(m_methodTransport, m_connection, std::move(settings),
                                 std::move(ownerAccountId), request, *serialized);
        if (const auto* operationError = std::get_if<javelin::jmap::OperationError>(&result))
        {
            if (operationError->protocolType.has_value())
            {
                if (const auto cacheError =
                        journal.restoreRejected(prepared.records, *operationError->protocolType))
                    co_return error(cacheError->message,
                                    javelin::jmap::OperationErrorCode::LocalStorageFailure);
            }
            else if (const auto cacheError = journal.transition(
                         prepared.records, javelin::jmap::sync::MutationStatus::Unknown))
                co_return error(cacheError->message,
                                javelin::jmap::OperationErrorCode::LocalStorageFailure);
            co_return *operationError;
        }
        if (prepared.records.empty())
        {
            const auto& copied = std::get<CopyObjectsResponse>(result).copied;
            const std::array domains{javelin::jmap::sync::ConsistencyDomain{
                .accountId = copied.accountId,
                .dataType = "ContactCard",
            }};
            co_return commitSetResult(m_connection, copied, domains);
        }
        co_return reconcileContactCopy(m_connection, m_repository, prepared.records,
                                       std::get<CopyObjectsResponse>(result));
    }

    QCoro::Task<ContactUploadResult>
    ContactService::uploadMedia(javelin::jmap::LiveConnectionSettings settings,
                                std::string ownerAccountId, std::string accountId,
                                QByteArray payload, std::string mediaType)
    {
        const auto sessionResult = loadSession(m_connection, ownerAccountId);
        if (const auto* loadError = std::get_if<javelin::jmap::OperationError>(&sessionResult))
        {
            co_return *loadError;
        }
        const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);
        QString uploadUrl = QString::fromStdString(session.uploadUrl);
        uploadUrl.replace(QStringLiteral("{accountId}"), QString::fromStdString(accountId));
        const auto result = co_await m_resourceTransport.send({
            .method = javelin::jmap::api::HttpMethod::Post,
            .url = QUrl{uploadUrl},
            .headers = {{.name = "Authorization",
                         .value =
                             QByteArray{"Bearer "} + QByteArray::fromStdString(settings.apiKey)},
                        {.name = "Content-Type", .value = QByteArray::fromStdString(mediaType)}},
            .body = std::move(payload),
            .cancellation = {},
            .dispatched = {},
        });
        if (const auto* transportError = std::get_if<javelin::jmap::api::TransportError>(&result))
        {
            co_return error(QString::fromStdString(transportError->message));
        }
        const auto& http = std::get<javelin::jmap::api::HttpResponse>(result);
        auto json = http.body.toStdString();
        detail::UploadResponse responseValue;
        if (glz::read<glz::opts{.error_on_unknown_keys = false}>(responseValue, json))
        {
            co_return error(QStringLiteral("Invalid JMAP upload response."));
        }
        co_return UploadedContactMedia{.accountId = std::move(responseValue.accountId),
                                       .blobId = std::move(responseValue.blobId),
                                       .mediaType = std::move(responseValue.type),
                                       .size = responseValue.size};
    }

    QCoro::Task<ContactDownloadResult>
    ContactService::downloadMedia(javelin::jmap::LiveConnectionSettings settings,
                                  std::string ownerAccountId, std::string accountId,
                                  std::string blobId, std::string mediaType)
    {
        const auto sessionResult = loadSession(m_connection, ownerAccountId);
        if (const auto* loadError = std::get_if<javelin::jmap::OperationError>(&sessionResult))
            co_return *loadError;
        const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);
        QString downloadUrl = QString::fromStdString(session.downloadUrl);
        const auto replaceTemplate = [&downloadUrl](const QString& name, const std::string& value)
        {
            downloadUrl.replace(
                name, QString::fromUtf8(QUrl::toPercentEncoding(QString::fromStdString(value))));
        };
        replaceTemplate(QStringLiteral("{accountId}"), accountId);
        replaceTemplate(QStringLiteral("{blobId}"), blobId);
        replaceTemplate(QStringLiteral("{name}"), "contact-photo");
        replaceTemplate(QStringLiteral("{type}"), mediaType);
        const auto result = co_await m_resourceTransport.send({
            .method = javelin::jmap::api::HttpMethod::Get,
            .url = QUrl{downloadUrl},
            .headers = {{.name = "Authorization",
                         .value =
                             QByteArray{"Bearer "} + QByteArray::fromStdString(settings.apiKey)}},
            .body = {},
            .cancellation = {},
            .dispatched = {},
        });
        if (const auto* transportError = std::get_if<javelin::jmap::api::TransportError>(&result))
            co_return error(QString::fromStdString(transportError->message));
        const auto& http = std::get<javelin::jmap::api::HttpResponse>(result);
        if (http.statusCode < 200 || http.statusCode >= 300)
        {
            co_return error(QStringLiteral("Contact photo download failed with HTTP status %1.")
                                .arg(http.statusCode));
        }
        co_return DownloadedContactMedia{.data = http.body, .mediaType = std::move(mediaType)};
    }
} // namespace javelin::jmap::contacts
