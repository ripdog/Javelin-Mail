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
                    .apiUrl = session.apiUrl};
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
                          const std::vector<std::string>& ids)
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
                if (!parsed.ok())
                    co_return error(QStringLiteral("Unable to parse ContactCard/get response."));
                for (const auto& card : parsed.value->list)
                {
                    auto contact = summarizeContact(accountId, card);
                    if (!contact.has_value())
                        co_return error(
                            QStringLiteral("The server returned an invalid ContactCard."));
                    result.contacts.push_back(std::move(*contact));
                }
                result.notFound.insert(result.notFound.end(), parsed.value->notFound.begin(),
                                       parsed.value->notFound.end());
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
                auto fetched = co_await fetchContactCards(methodTransport, settings, session,
                                                          accountId, changedIds);
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
                hasMoreChanges = changes.value->hasMoreChanges;
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
                if (item.callId == "contacts-set" && item.name != "error")
                {
                    actual = item;
                    break;
                }
            }
            if (!actual.has_value())
            {
                co_return error(QStringLiteral("The server rejected the Contacts change."));
            }
            const auto parsed = javelin::jmap::api::parseContactsSetResponse(actual->arguments);
            if (!parsed.ok())
            {
                co_return error(QStringLiteral("Invalid Contacts set response: %1")
                                    .arg(QString::fromStdString(parsed.error.value_or("unknown"))));
            }
            co_return *parsed.value;
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
                                          .createdId = createdId(result)};
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
                    if (!record.projectedDocument.has_value())
                        continue;
                    const auto transformed = javelin::jmap::api::applyPatchObject(
                        *record.projectedDocument, updated->second.json);
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
                    if (!record.projectedDocument.has_value())
                    {
                        continue;
                    }
                    const auto transformed = javelin::jmap::api::applyPatchObject(
                        *record.projectedDocument, updated->second.json);
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
            };
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
            javelin::jmap::cache::SyncStateRepository states{m_connection};
            const auto cachedState =
                states.find({.accountId = accountId, .objectType = "ContactCard", .queryKey = {}});
            if (const auto* stateError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&cachedState))
                co_return error(stateError->message);
            const auto& stateRecord =
                std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(cachedState);
            if (stateRecord.has_value() && !stateRecord->stateToken.empty())
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
            const auto addressBooksCurrent = fenceIsCurrent(m_connection, addressBookFence);
            const auto contactsCurrent = fenceIsCurrent(m_connection, contactFence);
            if (const auto* serviceError =
                    std::get_if<javelin::jmap::OperationError>(&addressBooksCurrent))
                co_return *serviceError;
            if (const auto* serviceError =
                    std::get_if<javelin::jmap::OperationError>(&contactsCurrent))
                co_return *serviceError;
            if (!std::get<bool>(addressBooksCurrent) || !std::get<bool>(contactsCurrent))
                continue;
            if (const auto cacheError = m_repository.replaceAll(
                    accountId, books.value->list, contacts, books.value->state, cards.value->state))
            {
                co_return error(cacheError->message);
            }
            ++summary.accountCount;
            summary.addressBookCount += books.value->list.size();
            summary.contactCount += contacts.size();
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

    QCoro::Task<ContactMutationResult>
    ContactService::copyContactCards(javelin::jmap::LiveConnectionSettings settings,
                                     std::string ownerAccountId,
                                     javelin::jmap::api::ContactCardCopyRequest request)
    {
        const auto accountId = request.accountId;
        std::vector<javelin::jmap::sync::ConsistencyDomain> affectedDomains{
            {.accountId = accountId, .dataType = "ContactCard"}};
        if (request.onSuccessDestroyOriginal && request.fromAccountId != accountId)
            affectedDomains.push_back(
                {.accountId = request.fromAccountId, .dataType = "ContactCard"});
        const auto result =
            co_await setObjects(m_methodTransport, m_connection, std::move(settings),
                                std::move(ownerAccountId), accountId, "ContactCard/copy",
                                javelin::jmap::api::serializeContactCardCopyRequest(request));
        if (const auto* operationError = std::get_if<javelin::jmap::OperationError>(&result))
            co_return *operationError;
        co_return commitSetResult(m_connection, std::get<javelin::jmap::api::SetResult>(result),
                                  affectedDomains);
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
