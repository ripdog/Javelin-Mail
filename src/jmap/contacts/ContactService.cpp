#include "jmap/contacts/ContactService.h"

#include "jmap/api/Error.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/RequestBuilder.h"
#include "jmap/api/Session.h"
#include "jmap/api/Transport.h"
#include "jmap/auth/Auth.h"
#include "jmap/cache/ContactRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/contacts/ContactTypes.h"

#include <glaze/glaze.hpp>

#include <QUrl>

#include <algorithm>
#include <limits>
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
            std::variant<javelin::jmap::api::Session, javelin::jmap::LiveRefreshError>;

        struct MethodFailure
        {
            javelin::jmap::api::MethodError error;
        };

        using ContactMethodResult = std::variant<javelin::jmap::api::MethodInvocation,
                                                 MethodFailure, javelin::jmap::LiveRefreshError>;

        struct CannotCalculateChanges
        {
        };

        struct AccountRefreshSummary
        {
            std::size_t addressBookCount = 0;
            std::size_t contactCount = 0;
        };

        using IncrementalRefreshResult = std::variant<AccountRefreshSummary, CannotCalculateChanges,
                                                      javelin::jmap::LiveRefreshError>;

        struct ContactGetBatch
        {
            std::vector<ContactSummary> contacts;
            std::vector<std::string> notFound;
        };

        using ContactGetBatchResult =
            std::variant<ContactGetBatch, javelin::jmap::LiveRefreshError>;

        [[nodiscard]] javelin::jmap::LiveRefreshError error(QString message,
                                                            const bool intervention = false)
        {
            return {.message = std::move(message), .requiresUserIntervention = intervention};
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
                return error(databaseError->message);
            }
            const auto& session = std::get<std::optional<javelin::jmap::api::Session>>(result);
            if (!session.has_value())
            {
                return error(QStringLiteral("No cached JMAP session is available."), true);
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

        [[nodiscard]] javelin::jmap::LiveRefreshError
        callError(const javelin::jmap::api::MethodCallerResult& result)
        {
            if (const auto* transport = std::get_if<javelin::jmap::api::TransportError>(&result))
            {
                return error(QStringLiteral("Contacts transport error: %1")
                                 .arg(QString::fromStdString(transport->message)));
            }
            if (const auto* auth = std::get_if<javelin::jmap::api::AuthError>(&result))
            {
                return error(QStringLiteral("Contacts authentication error: %1")
                                 .arg(QString::fromStdString(auth->message)),
                             true);
            }
            if (const auto* protocol = std::get_if<javelin::jmap::api::ProtocolError>(&result))
            {
                return error(QStringLiteral("Contacts protocol error: %1")
                                 .arg(QString::fromStdString(protocol->message)));
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
                        std::get_if<javelin::jmap::LiveRefreshError>(&callResult))
                    co_return *callFailure;
                if (const auto* methodFailure = std::get_if<MethodFailure>(&callResult))
                {
                    co_return error(
                        QStringLiteral("ContactCard/get failed: %1")
                            .arg(QString::fromStdString(methodFailure->error.description.value_or(
                                methodFailure->error.type))));
                }
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
                                    javelin::jmap::api::JmapMethodTransport& methodTransport,
                                    const javelin::jmap::LiveConnectionSettings& settings,
                                    const javelin::jmap::api::Session& session,
                                    const std::string& accountId, std::string state)
        {
            const auto booksArguments =
                javelin::jmap::api::serializeGetRequest({.accountId = accountId,
                                                         .ids = std::nullopt,
                                                         .idsReference = std::nullopt,
                                                         .properties = std::nullopt});
            if (!booksArguments.has_value())
                co_return error(QStringLiteral("Unable to serialize AddressBook/get."));
            auto booksCall = co_await callContactMethod(
                methodTransport, settings, session, accountId, "AddressBook/get", *booksArguments);
            if (const auto* callFailure = std::get_if<javelin::jmap::LiveRefreshError>(&booksCall))
                co_return *callFailure;
            if (const auto* methodFailure = std::get_if<MethodFailure>(&booksCall))
            {
                co_return error(
                    QStringLiteral("AddressBook/get failed: %1")
                        .arg(QString::fromStdString(
                            methodFailure->error.description.value_or(methodFailure->error.type))));
            }
            const auto books = javelin::jmap::api::parseAddressBookGetResponse(
                std::get<javelin::jmap::api::MethodInvocation>(booksCall).arguments);
            if (!books.ok())
                co_return error(QStringLiteral("Unable to parse AddressBook/get response."));
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
                        std::get_if<javelin::jmap::LiveRefreshError>(&changesCall))
                    co_return *callFailure;
                if (const auto* methodFailure = std::get_if<MethodFailure>(&changesCall))
                {
                    if (methodFailure->error.type == "cannotCalculateChanges")
                        co_return CannotCalculateChanges{};
                    co_return error(
                        QStringLiteral("ContactCard/changes failed: %1")
                            .arg(QString::fromStdString(methodFailure->error.description.value_or(
                                methodFailure->error.type))));
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
                if (const auto* fetchError = std::get_if<javelin::jmap::LiveRefreshError>(&fetched))
                    co_return *fetchError;
                auto batch = std::get<ContactGetBatch>(std::move(fetched));
                destroyed.insert(batch.notFound.begin(), batch.notFound.end());
                const std::vector<std::string> destroyedIds(destroyed.begin(), destroyed.end());
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
        createdId(const javelin::jmap::api::SetResult& result)
        {
            if (result.created.empty())
            {
                return std::nullopt;
            }
            auto json = result.created.begin()->second.json;
            detail::CreatedObject object;
            if (glz::read<glz::opts{.error_on_unknown_keys = false}>(object, json) ||
                object.id.empty())
            {
                return std::nullopt;
            }
            return object.id;
        }

        [[nodiscard]] QCoro::Task<ContactMutationResult>
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
            if (const auto* loadError =
                    std::get_if<javelin::jmap::LiveRefreshError>(&sessionResult))
            {
                co_return *loadError;
            }
            const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);
            const auto account = session.accounts.find(accountId);
            if (account == session.accounts.end() ||
                !account->second.accountCapabilities.contacts.has_value())
            {
                co_return error(QStringLiteral("This account does not support JMAP Contacts."),
                                true);
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
            co_return ContactMutationSummary{.accountId = accountId,
                                             .newState = parsed.value->newState,
                                             .createdId = createdId(*parsed.value)};
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
        if (const auto* loadError = std::get_if<javelin::jmap::LiveRefreshError>(&sessionResult))
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
                    m_repository, m_methodTransport, settings, session, accountId,
                    stateRecord->stateToken);
                if (const auto* incrementalError =
                        std::get_if<javelin::jmap::LiveRefreshError>(&incremental))
                    co_return *incrementalError;
                if (const auto* refreshed = std::get_if<AccountRefreshSummary>(&incremental))
                {
                    ++summary.accountCount;
                    summary.addressBookCount += refreshed->addressBookCount;
                    summary.contactCount += refreshed->contactCount;
                    continue;
                }
            }
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
        co_return co_await setObjects(m_methodTransport, m_connection, std::move(settings),
                                      std::move(ownerAccountId), accountId, "AddressBook/set",
                                      javelin::jmap::api::serializeAddressBookSetRequest(request));
    }

    QCoro::Task<ContactMutationResult>
    ContactService::setContactCards(javelin::jmap::LiveConnectionSettings settings,
                                    std::string ownerAccountId,
                                    javelin::jmap::api::ContactCardSetRequest request)
    {
        const auto accountId = request.accountId;
        co_return co_await setObjects(m_methodTransport, m_connection, std::move(settings),
                                      std::move(ownerAccountId), accountId, "ContactCard/set",
                                      javelin::jmap::api::serializeContactCardSetRequest(request));
    }

    QCoro::Task<ContactMutationResult>
    ContactService::copyContactCards(javelin::jmap::LiveConnectionSettings settings,
                                     std::string ownerAccountId,
                                     javelin::jmap::api::ContactCardCopyRequest request)
    {
        const auto accountId = request.accountId;
        co_return co_await setObjects(m_methodTransport, m_connection, std::move(settings),
                                      std::move(ownerAccountId), accountId, "ContactCard/copy",
                                      javelin::jmap::api::serializeContactCardCopyRequest(request));
    }

    QCoro::Task<ContactUploadResult>
    ContactService::uploadMedia(javelin::jmap::LiveConnectionSettings settings,
                                std::string ownerAccountId, std::string accountId,
                                QByteArray payload, std::string mediaType)
    {
        const auto sessionResult = loadSession(m_connection, ownerAccountId);
        if (const auto* loadError = std::get_if<javelin::jmap::LiveRefreshError>(&sessionResult))
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
        if (const auto* loadError = std::get_if<javelin::jmap::LiveRefreshError>(&sessionResult))
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
