#include "jmap/sieve/SieveService.h"

#include "jmap/api/MethodCaller.h"
#include "jmap/api/RequestBuilder.h"
#include "jmap/api/SessionClient.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/SieveRepository.h"
#include "jmap/sieve/SieveMutationJournal.h"
#include "jmap/sync/ConsistencyDomain.h"

#include <glaze/glaze.hpp>

#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <utility>

namespace javelin::jmap::sieve::detail
{
    struct GetRequest
    {
        std::string accountId;
        std::optional<std::vector<std::string>> ids;
    };

    struct GetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<SieveScript> list;
        std::vector<std::string> notFound;
    };

    struct UploadResponse
    {
        std::string accountId;
        std::string blobId;
        std::string type;
        std::uint64_t size = 0;
    };

    struct ValidateRequest
    {
        std::string accountId;
        std::string blobId;
    };

    struct SetError
    {
        std::string type;
        std::optional<std::string> description;
    };

    struct ValidateResponse
    {
        std::string accountId;
        std::optional<SetError> error;
    };

    struct ScriptUpdate
    {
        std::string blobId;
    };

    struct ScriptCreate
    {
        std::string name;
        std::string blobId;
    };

    struct SetRequest
    {
        std::string accountId;
        std::optional<std::string> ifInState;
        std::optional<std::unordered_map<std::string, ScriptCreate>> create;
        std::optional<std::unordered_map<std::string, ScriptUpdate>> update;
        std::optional<std::vector<std::string>> destroy;
        std::optional<std::string> onSuccessActivateScript;
        std::optional<bool> onSuccessDeactivateScript;
    };

    struct SetResponse
    {
        std::string accountId;
        std::string oldState;
        std::string newState;
        std::optional<std::unordered_map<std::string, SetError>> notUpdated;
        std::optional<std::unordered_map<std::string, SieveScript>> created;
        std::optional<std::unordered_map<std::string, std::optional<SieveScript>>> updated;
        std::optional<std::vector<std::string>> destroyed;
        std::optional<std::unordered_map<std::string, SetError>> notCreated;
        std::optional<std::unordered_map<std::string, SetError>> notDestroyed;
    };

    struct ResolvedContext
    {
        api::Session session;
        auth::AccountCredentials credentials;
        std::string sieveAccountId;
    };

    using ContextResult = std::variant<ResolvedContext, OperationError>;
    using UploadResult = std::variant<std::string, OperationError>;
} // namespace javelin::jmap::sieve::detail

template <> struct glz::meta<javelin::jmap::sieve::SieveScript>
{
    using T = javelin::jmap::sieve::SieveScript;
    static constexpr auto value =
        glz::object("id", &T::id, "name", &T::name, "blobId", &T::blobId, "isActive", &T::isActive);
};

template <> struct glz::meta<javelin::jmap::sieve::detail::GetRequest>
{
    using T = javelin::jmap::sieve::detail::GetRequest;
    static constexpr auto value = glz::object("accountId", &T::accountId, "ids", &T::ids);
};

template <> struct glz::meta<javelin::jmap::sieve::detail::GetResponse>
{
    using T = javelin::jmap::sieve::detail::GetResponse;
    static constexpr auto value = glz::object("accountId", &T::accountId, "state", &T::state,
                                              "list", &T::list, "notFound", &T::notFound);
};

template <> struct glz::meta<javelin::jmap::sieve::detail::UploadResponse>
{
    using T = javelin::jmap::sieve::detail::UploadResponse;
    static constexpr auto value = glz::object("accountId", &T::accountId, "blobId", &T::blobId,
                                              "type", &T::type, "size", &T::size);
};

template <> struct glz::meta<javelin::jmap::sieve::detail::ValidateRequest>
{
    using T = javelin::jmap::sieve::detail::ValidateRequest;
    static constexpr auto value = glz::object("accountId", &T::accountId, "blobId", &T::blobId);
};

template <> struct glz::meta<javelin::jmap::sieve::detail::SetError>
{
    using T = javelin::jmap::sieve::detail::SetError;
    static constexpr auto value = glz::object("type", &T::type, "description", &T::description);
};

template <> struct glz::meta<javelin::jmap::sieve::detail::ValidateResponse>
{
    using T = javelin::jmap::sieve::detail::ValidateResponse;
    static constexpr auto value = glz::object("accountId", &T::accountId, "error", &T::error);
};

template <> struct glz::meta<javelin::jmap::sieve::detail::ScriptUpdate>
{
    using T = javelin::jmap::sieve::detail::ScriptUpdate;
    static constexpr auto value = glz::object("blobId", &T::blobId);
};

template <> struct glz::meta<javelin::jmap::sieve::detail::ScriptCreate>
{
    using T = javelin::jmap::sieve::detail::ScriptCreate;
    static constexpr auto value = glz::object("name", &T::name, "blobId", &T::blobId);
};

template <> struct glz::meta<javelin::jmap::sieve::detail::SetRequest>
{
    using T = javelin::jmap::sieve::detail::SetRequest;
    static constexpr auto value = glz::object(
        "accountId", &T::accountId, "create", &T::create, "update", &T::update, "ifInState",
        &T::ifInState, "destroy", &T::destroy, "onSuccessActivateScript",
        &T::onSuccessActivateScript, "onSuccessDeactivateScript", &T::onSuccessDeactivateScript);
};

template <> struct glz::meta<javelin::jmap::sieve::detail::SetResponse>
{
    using T = javelin::jmap::sieve::detail::SetResponse;
    static constexpr auto value = glz::object(
        "accountId", &T::accountId, "oldState", &T::oldState, "newState", &T::newState,
        "notUpdated", &T::notUpdated, "created", &T::created, "notCreated", &T::notCreated,
        "updated", &T::updated, "destroyed", &T::destroyed, "notDestroyed", &T::notDestroyed);
};

namespace javelin::jmap::sieve
{
    namespace
    {
        constexpr std::string_view sieveGetMethod = "SieveScript/get";
        constexpr std::string_view sieveValidateMethod = "SieveScript/validate";
        constexpr std::string_view sieveSetMethod = "SieveScript/set";

        [[nodiscard]] OperationError error(OperationErrorCode code, QString message)
        {
            return {.code = code, .message = std::move(message)};
        }

        template <typename Value> [[nodiscard]] std::optional<std::string> serialize(Value value)
        {
            std::string json;
            if (glz::write_json(value, json))
                return std::nullopt;
            return json;
        }

        template <typename Value>
        [[nodiscard]] std::variant<Value, OperationError>
        parseMethodResponse(const api::ResponseEnvelope& envelope, const std::string_view callId,
                            const std::string_view methodName)
        {
            for (const auto& response : envelope.methodResponses)
            {
                if (response.callId != callId)
                    continue;
                if (response.name == "error")
                {
                    const auto parsed = api::parseMethodError(response.arguments);
                    if (parsed.value.has_value())
                        return operationError(*parsed.value);
                    return error(OperationErrorCode::ProtocolViolation,
                                 QStringLiteral("The server returned an invalid method error."));
                }
                if (response.name != methodName)
                    continue;
                Value value;
                auto json = response.arguments;
                if (glz::read<glz::opts{.error_on_unknown_keys = false}>(value, json))
                    return error(OperationErrorCode::ProtocolViolation,
                                 QStringLiteral("The server returned an invalid Sieve response."));
                return value;
            }
            return error(OperationErrorCode::ProtocolViolation,
                         QStringLiteral("The server omitted the Sieve response."));
        }

        [[nodiscard]] auth::AccountCredentials credentials(const LiveConnectionSettings& settings,
                                                           const std::string& ownerAccountId)
        {
            return {.accountId = ownerAccountId,
                    .emailAddress = settings.loginEmail,
                    .sessionUrl = settings.sessionUrl,
                    .token = {.accessToken = settings.apiKey,
                              .refreshToken = std::nullopt,
                              .expiry = std::nullopt}};
        }

        [[nodiscard]] QCoro::Task<detail::ContextResult>
        resolveContext(api::AbstractTransport& transport, LiveConnectionSettings settings,
                       std::string ownerAccountId)
        {
            api::SessionClient client{transport};
            auto accountCredentials = credentials(settings, ownerAccountId);
            auto result = co_await client.discover(
                {.credentials = accountCredentials, .requiredCapabilities = {.sieve = true}});
            if (const auto* value = std::get_if<api::Session>(&result))
            {
                co_return detail::ResolvedContext{.session = *value,
                                                  .credentials = std::move(accountCredentials),
                                                  .sieveAccountId =
                                                      *value->primaryAccounts.sieveAccountId};
            }
            if (const auto* authError = std::get_if<api::AuthError>(&result))
                co_return operationError(*authError);
            if (const auto* protocolError = std::get_if<api::ProtocolError>(&result))
                co_return operationError(*protocolError);
            co_return operationError(std::get<api::TransportError>(result));
        }

        [[nodiscard]] api::ApiRequestContext apiContext(const detail::ResolvedContext& context)
        {
            return {.credentials = context.credentials, .apiUrl = context.session.apiUrl};
        }

        [[nodiscard]] QCoro::Task<std::variant<api::ResponseEnvelope, OperationError>>
        call(api::JmapMethodTransport& transport, const detail::ResolvedContext& context,
             std::string methodName, std::string arguments, std::string callId)
        {
            api::RequestBuilder builder;
            builder.useCore().useCapability(std::string{api::sieveCapabilityUri});
            static_cast<void>(builder.call(
                api::MethodRequest<detail::GetResponse>{.name = std::move(methodName),
                                                        .arguments = std::move(arguments)},
                std::move(callId)));
            api::MethodCaller caller{transport};
            auto result = co_await caller.call(apiContext(context), builder);
            if (const auto* envelope = std::get_if<api::ResponseEnvelope>(&result))
                co_return *envelope;
            if (const auto* authError = std::get_if<api::AuthError>(&result))
                co_return operationError(*authError);
            if (const auto* transportError = std::get_if<api::TransportError>(&result))
                co_return operationError(*transportError);
            co_return operationError(std::get<api::ProtocolError>(result));
        }

        [[nodiscard]] QString expandUrl(std::string urlTemplate, const std::string& accountId,
                                        const SieveScript& script)
        {
            QString url = QString::fromStdString(std::move(urlTemplate));
            const auto encoded = [](const std::string& value)
            { return QString::fromUtf8(QUrl::toPercentEncoding(QString::fromStdString(value))); };
            url.replace(QStringLiteral("{accountId}"), encoded(accountId));
            url.replace(QStringLiteral("{blobId}"), encoded(script.blobId));
            url.replace(QStringLiteral("{name}"), encoded(script.name + ".siv"));
            url.replace(QStringLiteral("{type}"), QStringLiteral("application%2Fsieve"));
            return url;
        }

        [[nodiscard]] QCoro::Task<detail::UploadResult>
        upload(api::AbstractTransport& transport, const detail::ResolvedContext& context,
               QByteArray content)
        {
            QString url = QString::fromStdString(context.session.uploadUrl);
            url.replace(QStringLiteral("{accountId}"),
                        QString::fromUtf8(QUrl::toPercentEncoding(
                            QString::fromStdString(context.sieveAccountId))));
            auto result = co_await transport.send({
                .method = api::HttpMethod::Post,
                .url = QUrl{url},
                .headers = {{.name = "Authorization",
                             .value =
                                 QByteArray{"Bearer "} +
                                 QByteArray::fromStdString(context.credentials.token.accessToken)},
                            {.name = "Accept", .value = "application/json"},
                            {.name = "Content-Type", .value = "application/sieve"}},
                .body = std::move(content),
            });
            if (const auto* transportError = std::get_if<api::TransportError>(&result))
                co_return operationError(*transportError);
            detail::UploadResponse response;
            auto json = std::get<api::HttpResponse>(result).body.toStdString();
            if (glz::read<glz::opts{.error_on_unknown_keys = false}>(response, json) ||
                response.blobId.empty())
                co_return error(OperationErrorCode::ProtocolViolation,
                                QStringLiteral("The server returned an invalid upload response."));
            co_return std::move(response.blobId);
        }

        [[nodiscard]] QCoro::Task<SieveValidationResult>
        validateBlob(api::JmapMethodTransport& transport, const detail::ResolvedContext& context,
                     const std::string& blobId)
        {
            const auto arguments = serialize(
                detail::ValidateRequest{.accountId = context.sieveAccountId, .blobId = blobId});
            if (!arguments)
                co_return error(OperationErrorCode::ProtocolViolation,
                                QStringLiteral("Failed to encode the validation request."));
            auto called = co_await call(transport, context, std::string{sieveValidateMethod},
                                        *arguments, "sieve-validate");
            if (const auto* callError = std::get_if<OperationError>(&called))
                co_return *callError;
            auto parsed = parseMethodResponse<detail::ValidateResponse>(
                std::get<api::ResponseEnvelope>(called), "sieve-validate", sieveValidateMethod);
            if (const auto* parseError = std::get_if<OperationError>(&parsed))
                co_return *parseError;
            const auto& response = std::get<detail::ValidateResponse>(parsed);
            if (!response.error)
                co_return SieveValidation{.valid = true,
                                          .message = QStringLiteral("The script is valid.")};
            co_return SieveValidation{
                .valid = false,
                .message = response.error->description
                               ? QString::fromStdString(*response.error->description)
                               : QStringLiteral("The server rejected the Sieve script.")};
        }

        [[nodiscard]] std::variant<std::vector<SieveScript>, OperationError>
        cachedScripts(cache::SieveRepository& repository, const std::string& accountId)
        {
            const auto listed = repository.list(accountId);
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&listed))
                return operationError(*cacheError);
            return std::get<std::vector<SieveScript>>(listed);
        }

        [[nodiscard]] SieveMutationRecord mutationRecord(
            const std::string& accountId, std::string objectId, const SieveMutationKind kind,
            std::vector<SieveScript> baseScripts, std::vector<SieveScript> projectedScripts,
            std::optional<std::string> baseState, std::optional<std::string> operationGroupId)
        {
            return {
                .mutationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
                .operationGroupId = std::move(operationGroupId),
                .accountId = accountId,
                .objectId = std::move(objectId),
                .kind = kind,
                .status = sync::MutationStatus::Pending,
                .baseScripts = std::move(baseScripts),
                .projectedScripts = std::move(projectedScripts),
                .baseState = std::move(baseState),
                .acceptedState = std::nullopt,
                .errorJson = std::nullopt,
            };
        }

        [[nodiscard]] std::optional<OperationError>
        acceptMutation(cache::DatabaseConnection& connection, cache::SieveRepository& repository,
                       const SieveMutationRecord& record,
                       const std::vector<SieveScript>& acceptedScripts,
                       const std::string& acceptedState)
        {
            auto transactionResult = sync::MutationProjectionTransaction::begin(
                connection, QStringLiteral("Accept Sieve mutation"));
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&transactionResult))
                return operationError(*cacheError);
            auto transaction =
                std::get<sync::MutationProjectionTransaction>(std::move(transactionResult));
            if (const auto cacheError = transaction.transition(
                    record.mutationId, sync::MutationStatus::Accepted, acceptedState))
                return operationError(*cacheError);
            const std::array domains{sync::ConsistencyDomain{
                .accountId = record.accountId,
                .dataType = "SieveScript",
            }};
            if (const auto cacheError = transaction.advance(domains))
                return operationError(*cacheError);
            if (const auto cacheError =
                    repository.replaceAll(transaction.cacheTransaction(), record.accountId,
                                          acceptedScripts, acceptedState))
                return operationError(*cacheError);
            if (const auto cacheError = transaction.remove(record.mutationId))
                return operationError(*cacheError);
            if (const auto cacheError = transaction.commit())
                return operationError(*cacheError);
            return std::nullopt;
        }

        [[nodiscard]] bool sameScripts(std::vector<SieveScript> left,
                                       std::vector<SieveScript> right)
        {
            const auto byId = [](const SieveScript& first, const SieveScript& second)
            { return first.id < second.id; };
            std::ranges::sort(left, byId);
            std::ranges::sort(right, byId);
            return std::ranges::equal(left, right,
                                      [](const SieveScript& first, const SieveScript& second)
                                      {
                                          return first.id == second.id &&
                                                 first.name == second.name &&
                                                 first.blobId == second.blobId &&
                                                 first.isActive == second.isActive;
                                      });
        }

        void correlateCreatedScripts(std::vector<SieveScript>& projected,
                                     const std::vector<SieveScript>& serverScripts)
        {
            for (auto& script : projected)
            {
                if (!script.id.starts_with("local-"))
                    continue;
                const auto found = std::ranges::find_if(
                    serverScripts, [&script](const SieveScript& server)
                    { return server.name == script.name && server.blobId == script.blobId; });
                if (found != serverScripts.end())
                    script = *found;
            }
        }
    } // namespace

    SieveService::SieveService(cache::DatabaseConnection& connection,
                               api::AbstractTransport& resourceTransport,
                               api::JmapMethodTransport& methodTransport)
        : m_connection(connection), m_resourceTransport(resourceTransport),
          m_methodTransport(methodTransport)
    {
    }

    QCoro::Task<SieveListResult> SieveService::list(LiveConnectionSettings settings,
                                                    std::string ownerAccountId) const
    {
        auto contextResult = co_await resolveContext(m_resourceTransport, std::move(settings),
                                                     std::move(ownerAccountId));
        if (const auto* contextError = std::get_if<OperationError>(&contextResult))
            co_return *contextError;
        const auto& context = std::get<detail::ResolvedContext>(contextResult);
        sync::ConsistencyDomainRepository consistency{m_connection};
        const auto fenceResult = consistency.captureRefresh(
            {.accountId = context.sieveAccountId, .dataType = "SieveScript"});
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&fenceResult))
            co_return operationError(*cacheError);
        const auto fence = std::get<sync::RefreshFence>(fenceResult);
        const auto arguments =
            serialize(detail::GetRequest{.accountId = context.sieveAccountId, .ids = std::nullopt});
        if (!arguments)
            co_return error(OperationErrorCode::ProtocolViolation,
                            QStringLiteral("Failed to encode the script list request."));
        auto called = co_await call(m_methodTransport, context, std::string{sieveGetMethod},
                                    *arguments, "sieve-list");
        if (const auto* callError = std::get_if<OperationError>(&called))
            co_return *callError;
        auto parsed = parseMethodResponse<detail::GetResponse>(
            std::get<api::ResponseEnvelope>(called), "sieve-list", sieveGetMethod);
        if (const auto* parseError = std::get_if<OperationError>(&parsed))
            co_return *parseError;
        const auto& responseValue = std::get<detail::GetResponse>(parsed);
        const auto isCurrent = consistency.isCurrent(fence);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&isCurrent))
            co_return operationError(*cacheError);
        cache::SieveRepository repository{m_connection};
        if (!std::get<bool>(isCurrent))
        {
            const auto cached = repository.list(context.sieveAccountId);
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&cached))
                co_return operationError(*cacheError);
            co_return std::get<std::vector<SieveScript>>(cached);
        }
        SieveMutationJournal mutationJournal{m_connection, repository};
        const auto activeResult = mutationJournal.listActive(context.sieveAccountId);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&activeResult))
            co_return operationError(*cacheError);
        const auto& active = std::get<std::vector<SieveMutationRecord>>(activeResult);
        auto visibleScripts = responseValue.list;
        std::vector<const SieveMutationRecord*> acceptedUnknown;
        for (const auto& mutation : active)
        {
            auto projected = mutation.projectedScripts;
            correlateCreatedScripts(projected, responseValue.list);
            if (mutation.status == sync::MutationStatus::Unknown &&
                sameScripts(projected, responseValue.list))
                acceptedUnknown.push_back(&mutation);
            visibleScripts = std::move(projected);
        }
        auto transactionResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Rebase Sieve refresh"));
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&transactionResult))
            co_return operationError(*cacheError);
        auto transaction =
            std::get<sync::MutationProjectionTransaction>(std::move(transactionResult));
        if (!acceptedUnknown.empty())
        {
            const std::array domains{sync::ConsistencyDomain{
                .accountId = context.sieveAccountId,
                .dataType = "SieveScript",
            }};
            if (const auto cacheError = transaction.advance(domains))
                co_return operationError(*cacheError);
            for (const auto* mutation : acceptedUnknown)
            {
                if (const auto cacheError = transaction.transition(
                        mutation->mutationId, sync::MutationStatus::Accepted, responseValue.state))
                    co_return operationError(*cacheError);
                if (const auto cacheError = transaction.remove(mutation->mutationId))
                    co_return operationError(*cacheError);
            }
        }
        if (const auto cacheError =
                repository.replaceAll(transaction.cacheTransaction(), context.sieveAccountId,
                                      visibleScripts, responseValue.state))
            co_return operationError(*cacheError);
        if (const auto cacheError = transaction.commit())
            co_return operationError(*cacheError);
        co_return visibleScripts;
    }

    QCoro::Task<SieveContentResult> SieveService::load(LiveConnectionSettings settings,
                                                       std::string ownerAccountId,
                                                       SieveScript script) const
    {
        auto contextResult = co_await resolveContext(m_resourceTransport, std::move(settings),
                                                     std::move(ownerAccountId));
        if (const auto* contextError = std::get_if<OperationError>(&contextResult))
            co_return *contextError;
        const auto& context = std::get<detail::ResolvedContext>(contextResult);
        auto result = co_await m_resourceTransport.send({
            .method = api::HttpMethod::Get,
            .url = QUrl{expandUrl(context.session.downloadUrl, context.sieveAccountId, script)},
            .headers = {{.name = "Authorization",
                         .value = QByteArray{"Bearer "} +
                                  QByteArray::fromStdString(context.credentials.token.accessToken)},
                        {.name = "Accept", .value = "application/sieve"}},
            .body = {},
        });
        if (const auto* transportError = std::get_if<api::TransportError>(&result))
            co_return operationError(*transportError);
        co_return std::get<api::HttpResponse>(std::move(result)).body;
    }

    QCoro::Task<SieveValidationResult> SieveService::validate(LiveConnectionSettings settings,
                                                              std::string ownerAccountId,
                                                              QByteArray content) const
    {
        auto contextResult = co_await resolveContext(m_resourceTransport, std::move(settings),
                                                     std::move(ownerAccountId));
        if (const auto* contextError = std::get_if<OperationError>(&contextResult))
            co_return *contextError;
        const auto& context = std::get<detail::ResolvedContext>(contextResult);
        auto uploaded = co_await upload(m_resourceTransport, context, std::move(content));
        if (const auto* uploadError = std::get_if<OperationError>(&uploaded))
            co_return *uploadError;
        co_return co_await validateBlob(m_methodTransport, context,
                                        std::get<std::string>(uploaded));
    }

    QCoro::Task<SieveSaveResult>
    SieveService::save(LiveConnectionSettings settings, std::string ownerAccountId,
                       SieveScript script, QByteArray content,
                       std::optional<std::string> operationGroupId) const
    {
        auto contextResult = co_await resolveContext(m_resourceTransport, std::move(settings),
                                                     std::move(ownerAccountId));
        if (const auto* contextError = std::get_if<OperationError>(&contextResult))
            co_return *contextError;
        const auto& context = std::get<detail::ResolvedContext>(contextResult);
        auto uploaded = co_await upload(m_resourceTransport, context, std::move(content));
        if (const auto* uploadError = std::get_if<OperationError>(&uploaded))
            co_return *uploadError;
        const auto& blobId = std::get<std::string>(uploaded);
        auto validation = co_await validateBlob(m_methodTransport, context, blobId);
        if (const auto* validationError = std::get_if<OperationError>(&validation))
            co_return *validationError;
        const auto& validationValue = std::get<SieveValidation>(validation);
        if (!validationValue.valid)
            co_return error(OperationErrorCode::InvalidUserInput, validationValue.message);

        cache::SieveRepository repository{m_connection};
        const auto baseResult = cachedScripts(repository, context.sieveAccountId);
        if (const auto* cacheError = std::get_if<OperationError>(&baseResult))
            co_return *cacheError;
        auto baseScripts = std::get<std::vector<SieveScript>>(baseResult);
        auto projectedScripts = baseScripts;
        const bool creating = script.id.empty();
        const auto serverScriptId = script.id;
        const auto temporaryId =
            creating ? std::string{"local-"} +
                           QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString()
                     : script.id;
        if (creating)
        {
            script.id = temporaryId;
            script.blobId = blobId;
            projectedScripts.push_back(script);
        }
        else
        {
            const auto found = std::ranges::find(projectedScripts, script.id, &SieveScript::id);
            if (found == projectedScripts.end())
                co_return error(OperationErrorCode::LocalStorageFailure,
                                QStringLiteral("The cached Sieve script is unavailable."));
            found->blobId = blobId;
        }
        const auto stateResult = repository.state(context.sieveAccountId);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&stateResult))
            co_return operationError(*cacheError);
        auto mutation = mutationRecord(
            context.sieveAccountId, temporaryId,
            creating ? SieveMutationKind::Create : SieveMutationKind::Update,
            std::move(baseScripts), projectedScripts,
            std::get<std::optional<std::string>>(stateResult), std::move(operationGroupId));
        SieveMutationJournal journal{m_connection, repository};
        if (const auto cacheError = journal.queue(mutation))
            co_return operationError(*cacheError);
        if (const auto cacheError = journal.transition(mutation, sync::MutationStatus::InFlight))
            co_return operationError(*cacheError);

        constexpr std::string_view creationId = "new-script";
        const auto arguments = serialize(detail::SetRequest{
            .accountId = context.sieveAccountId,
            .ifInState = mutation.baseState,
            .create = creating ? std::optional<std::unordered_map<
                                     std::string, detail::ScriptCreate>>{{{std::string{creationId},
                                                                           {.name = script.name,
                                                                            .blobId = blobId}}}}
                               : std::nullopt,
            .update = creating ? std::nullopt
                               : std::optional<std::unordered_map<
                                     std::string, detail::ScriptUpdate>>{{{serverScriptId,
                                                                           {.blobId = blobId}}}},
            .destroy = std::nullopt,
            .onSuccessActivateScript = std::nullopt,
            .onSuccessDeactivateScript = std::nullopt,
        });
        if (!arguments)
            co_return error(OperationErrorCode::ProtocolViolation,
                            QStringLiteral("Failed to encode the script update request."));
        auto called = co_await call(m_methodTransport, context, std::string{sieveSetMethod},
                                    *arguments, "sieve-save");
        if (const auto* callError = std::get_if<OperationError>(&called))
        {
            if (const auto cacheError = journal.transition(mutation, sync::MutationStatus::Unknown))
                co_return operationError(*cacheError);
            co_return *callError;
        }
        auto parsed = parseMethodResponse<detail::SetResponse>(
            std::get<api::ResponseEnvelope>(called), "sieve-save", sieveSetMethod);
        if (const auto* parseError = std::get_if<OperationError>(&parsed))
        {
            if (parseError->protocolType.has_value())
            {
                if (const auto cacheError =
                        journal.restoreRejected(mutation, std::nullopt, *parseError->protocolType))
                    co_return operationError(*cacheError);
            }
            else if (const auto cacheError =
                         journal.transition(mutation, sync::MutationStatus::Unknown))
                co_return operationError(*cacheError);
            co_return *parseError;
        }
        const auto& response = std::get<detail::SetResponse>(parsed);
        if (creating)
        {
            if (response.notCreated)
            {
                const auto rejected = response.notCreated->find(std::string{creationId});
                if (rejected != response.notCreated->end())
                {
                    if (const auto cacheError =
                            journal.restoreRejected(mutation, std::nullopt, rejected->second.type))
                        co_return operationError(*cacheError);
                    co_return error(rejected->second.type == "invalidSieve"
                                        ? OperationErrorCode::InvalidUserInput
                                        : OperationErrorCode::ProtocolViolation,
                                    rejected->second.description
                                        ? QString::fromStdString(*rejected->second.description)
                                        : QStringLiteral("The server rejected the new script."));
                }
            }
            if (!response.created)
            {
                if (const auto cacheError =
                        journal.transition(mutation, sync::MutationStatus::Unknown))
                    co_return operationError(*cacheError);
                co_return error(OperationErrorCode::ProtocolViolation,
                                QStringLiteral("The server did not return the new script."));
            }
            const auto created = response.created->find(std::string{creationId});
            if (created == response.created->end())
            {
                if (const auto cacheError =
                        journal.transition(mutation, sync::MutationStatus::Unknown))
                    co_return operationError(*cacheError);
                co_return error(OperationErrorCode::ProtocolViolation,
                                QStringLiteral("The server did not return the new script."));
            }
            auto createdScript = created->second;
            if (createdScript.id.empty())
            {
                if (const auto cacheError =
                        journal.transition(mutation, sync::MutationStatus::Unknown))
                    co_return operationError(*cacheError);
                co_return error(OperationErrorCode::ProtocolViolation,
                                QStringLiteral("The server omitted the new script id."));
            }
            if (createdScript.name.empty())
                createdScript.name = script.name;
            if (createdScript.blobId.empty())
                createdScript.blobId = blobId;
            auto acceptedScripts = projectedScripts;
            std::erase_if(acceptedScripts, [&temporaryId](const SieveScript& value)
                          { return value.id == temporaryId; });
            acceptedScripts.push_back(createdScript);
            if (const auto acceptedError = acceptMutation(m_connection, repository, mutation,
                                                          acceptedScripts, response.newState))
                co_return *acceptedError;
            co_return createdScript;
        }
        if (response.notUpdated)
        {
            const auto rejected = response.notUpdated->find(serverScriptId);
            if (rejected != response.notUpdated->end())
            {
                if (const auto cacheError =
                        journal.restoreRejected(mutation, std::nullopt, rejected->second.type))
                    co_return operationError(*cacheError);
                co_return error(rejected->second.type == "invalidSieve"
                                    ? OperationErrorCode::InvalidUserInput
                                    : OperationErrorCode::ProtocolViolation,
                                rejected->second.description
                                    ? QString::fromStdString(*rejected->second.description)
                                    : QStringLiteral("The server rejected the script update."));
            }
        }
        if (!response.updated || !response.updated->contains(serverScriptId))
        {
            if (const auto cacheError = journal.transition(mutation, sync::MutationStatus::Unknown))
                co_return operationError(*cacheError);
            co_return error(OperationErrorCode::ProtocolViolation,
                            QStringLiteral("The server did not confirm the script update."));
        }
        script.blobId = blobId;
        auto acceptedScripts = projectedScripts;
        if (response.updated)
        {
            const auto updated = response.updated->find(serverScriptId);
            if (updated != response.updated->end() && updated->second.has_value())
            {
                const auto found =
                    std::ranges::find(acceptedScripts, serverScriptId, &SieveScript::id);
                if (found != acceptedScripts.end())
                {
                    if (!updated->second->name.empty())
                        found->name = updated->second->name;
                    if (!updated->second->blobId.empty())
                        found->blobId = updated->second->blobId;
                    found->isActive = updated->second->isActive;
                    script = *found;
                }
            }
        }
        if (const auto acceptedError = acceptMutation(m_connection, repository, mutation,
                                                      acceptedScripts, response.newState))
            co_return *acceptedError;
        co_return script;
    }

    QCoro::Task<SieveDeleteResult>
    SieveService::remove(LiveConnectionSettings settings, std::string ownerAccountId,
                         SieveScript script, std::optional<std::string> operationGroupId) const
    {
        if (script.id.empty())
            co_return std::monostate{};
        auto contextResult = co_await resolveContext(m_resourceTransport, std::move(settings),
                                                     std::move(ownerAccountId));
        if (const auto* contextError = std::get_if<OperationError>(&contextResult))
            co_return *contextError;
        const auto& context = std::get<detail::ResolvedContext>(contextResult);
        cache::SieveRepository repository{m_connection};
        const auto baseResult = cachedScripts(repository, context.sieveAccountId);
        if (const auto* cacheError = std::get_if<OperationError>(&baseResult))
            co_return *cacheError;
        auto baseScripts = std::get<std::vector<SieveScript>>(baseResult);
        if (std::ranges::find(baseScripts, script.id, &SieveScript::id) == baseScripts.end())
            co_return error(OperationErrorCode::LocalStorageFailure,
                            QStringLiteral("The cached Sieve script is unavailable."));
        auto projectedScripts = baseScripts;
        std::erase_if(projectedScripts,
                      [&script](const SieveScript& value) { return value.id == script.id; });
        const auto stateResult = repository.state(context.sieveAccountId);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&stateResult))
            co_return operationError(*cacheError);
        auto mutation = mutationRecord(context.sieveAccountId, script.id,
                                       SieveMutationKind::Destroy, baseScripts, projectedScripts,
                                       std::get<std::optional<std::string>>(stateResult),
                                       std::move(operationGroupId));
        SieveMutationJournal journal{m_connection, repository};
        if (const auto cacheError = journal.queue(mutation))
            co_return operationError(*cacheError);
        if (const auto cacheError = journal.transition(mutation, sync::MutationStatus::InFlight))
            co_return operationError(*cacheError);

        if (script.isActive)
        {
            const auto deactivateArguments = serialize(detail::SetRequest{
                .accountId = context.sieveAccountId,
                .ifInState = mutation.baseState,
                .create = std::nullopt,
                .update = std::nullopt,
                .destroy = std::nullopt,
                .onSuccessActivateScript = std::nullopt,
                .onSuccessDeactivateScript = true,
            });
            if (!deactivateArguments)
                co_return error(
                    OperationErrorCode::ProtocolViolation,
                    QStringLiteral("Failed to encode the script deactivation request."));
            auto deactivated =
                co_await call(m_methodTransport, context, std::string{sieveSetMethod},
                              *deactivateArguments, "sieve-deactivate");
            if (const auto* callError = std::get_if<OperationError>(&deactivated))
            {
                if (const auto cacheError =
                        journal.transition(mutation, sync::MutationStatus::Unknown))
                    co_return operationError(*cacheError);
                co_return *callError;
            }
            auto parsed = parseMethodResponse<detail::SetResponse>(
                std::get<api::ResponseEnvelope>(deactivated), "sieve-deactivate", sieveSetMethod);
            if (const auto* parseError = std::get_if<OperationError>(&parsed))
            {
                if (parseError->protocolType.has_value())
                {
                    if (const auto cacheError = journal.restoreRejected(mutation, std::nullopt,
                                                                        *parseError->protocolType))
                        co_return operationError(*cacheError);
                }
                else if (const auto cacheError =
                             journal.transition(mutation, sync::MutationStatus::Unknown))
                    co_return operationError(*cacheError);
                co_return *parseError;
            }
            const auto& response = std::get<detail::SetResponse>(parsed);
            mutation.baseState = response.newState;
            const auto base = std::ranges::find(mutation.baseScripts, script.id, &SieveScript::id);
            if (base != mutation.baseScripts.end())
                base->isActive = false;
        }

        const auto destroyArguments = serialize(detail::SetRequest{
            .accountId = context.sieveAccountId,
            .ifInState = mutation.baseState,
            .create = std::nullopt,
            .update = std::nullopt,
            .destroy = std::vector<std::string>{script.id},
            .onSuccessActivateScript = std::nullopt,
            .onSuccessDeactivateScript = std::nullopt,
        });
        if (!destroyArguments)
            co_return error(OperationErrorCode::ProtocolViolation,
                            QStringLiteral("Failed to encode the script deletion request."));
        auto destroyed = co_await call(m_methodTransport, context, std::string{sieveSetMethod},
                                       *destroyArguments, "sieve-delete");
        if (const auto* callError = std::get_if<OperationError>(&destroyed))
        {
            if (const auto cacheError = journal.transition(mutation, sync::MutationStatus::Unknown))
                co_return operationError(*cacheError);
            co_return *callError;
        }
        auto parsed = parseMethodResponse<detail::SetResponse>(
            std::get<api::ResponseEnvelope>(destroyed), "sieve-delete", sieveSetMethod);
        if (const auto* parseError = std::get_if<OperationError>(&parsed))
        {
            if (parseError->protocolType.has_value())
            {
                if (const auto cacheError = journal.restoreRejected(mutation, mutation.baseState,
                                                                    *parseError->protocolType))
                    co_return operationError(*cacheError);
            }
            else if (const auto cacheError =
                         journal.transition(mutation, sync::MutationStatus::Unknown))
                co_return operationError(*cacheError);
            co_return *parseError;
        }
        const auto& response = std::get<detail::SetResponse>(parsed);
        if (response.notDestroyed)
        {
            const auto rejected = response.notDestroyed->find(script.id);
            if (rejected != response.notDestroyed->end())
            {
                if (const auto cacheError = journal.restoreRejected(mutation, mutation.baseState,
                                                                    rejected->second.type))
                    co_return operationError(*cacheError);
                co_return error(OperationErrorCode::ProtocolViolation,
                                rejected->second.description
                                    ? QString::fromStdString(*rejected->second.description)
                                    : QStringLiteral("The server rejected the script deletion."));
            }
        }
        if (!response.destroyed ||
            std::ranges::find(*response.destroyed, script.id) == response.destroyed->end())
        {
            if (const auto cacheError = journal.transition(mutation, sync::MutationStatus::Unknown))
                co_return operationError(*cacheError);
            co_return error(OperationErrorCode::ProtocolViolation,
                            QStringLiteral("The server did not confirm the script deletion."));
        }
        if (const auto acceptedError = acceptMutation(m_connection, repository, mutation,
                                                      projectedScripts, response.newState))
            co_return *acceptedError;
        co_return std::monostate{};
    }

    QCoro::Task<SieveActivationResult>
    SieveService::setActive(LiveConnectionSettings settings, std::string ownerAccountId,
                            SieveScript script, const bool active,
                            std::optional<std::string> operationGroupId) const
    {
        if (script.id.empty())
            co_return error(OperationErrorCode::ProtocolViolation,
                            QStringLiteral("Save the script before activating it."));
        auto contextResult = co_await resolveContext(m_resourceTransport, std::move(settings),
                                                     std::move(ownerAccountId));
        if (const auto* contextError = std::get_if<OperationError>(&contextResult))
            co_return *contextError;
        const auto& context = std::get<detail::ResolvedContext>(contextResult);
        cache::SieveRepository repository{m_connection};
        const auto baseResult = cachedScripts(repository, context.sieveAccountId);
        if (const auto* cacheError = std::get_if<OperationError>(&baseResult))
            co_return *cacheError;
        auto baseScripts = std::get<std::vector<SieveScript>>(baseResult);
        auto projectedScripts = baseScripts;
        const auto target = std::ranges::find(projectedScripts, script.id, &SieveScript::id);
        if (target == projectedScripts.end())
            co_return error(OperationErrorCode::LocalStorageFailure,
                            QStringLiteral("The cached Sieve script is unavailable."));
        if (active)
        {
            for (auto& value : projectedScripts)
                value.isActive = value.id == script.id;
        }
        else
            target->isActive = false;
        const auto stateResult = repository.state(context.sieveAccountId);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&stateResult))
            co_return operationError(*cacheError);
        auto mutation = mutationRecord(
            context.sieveAccountId, script.id, SieveMutationKind::Activate, std::move(baseScripts),
            projectedScripts, std::get<std::optional<std::string>>(stateResult),
            std::move(operationGroupId));
        SieveMutationJournal journal{m_connection, repository};
        if (const auto cacheError = journal.queue(mutation))
            co_return operationError(*cacheError);
        if (const auto cacheError = journal.transition(mutation, sync::MutationStatus::InFlight))
            co_return operationError(*cacheError);
        const auto arguments = serialize(detail::SetRequest{
            .accountId = context.sieveAccountId,
            .ifInState = mutation.baseState,
            .create = std::nullopt,
            .update = std::nullopt,
            .destroy = std::nullopt,
            .onSuccessActivateScript = active ? std::optional{script.id} : std::nullopt,
            .onSuccessDeactivateScript = active ? std::nullopt : std::optional{true},
        });
        if (!arguments)
            co_return error(OperationErrorCode::ProtocolViolation,
                            QStringLiteral("Failed to encode the script activation request."));
        auto called = co_await call(m_methodTransport, context, std::string{sieveSetMethod},
                                    *arguments, "sieve-active");
        if (const auto* callError = std::get_if<OperationError>(&called))
        {
            if (const auto cacheError = journal.transition(mutation, sync::MutationStatus::Unknown))
                co_return operationError(*cacheError);
            co_return *callError;
        }
        auto parsed = parseMethodResponse<detail::SetResponse>(
            std::get<api::ResponseEnvelope>(called), "sieve-active", sieveSetMethod);
        if (const auto* parseError = std::get_if<OperationError>(&parsed))
        {
            if (parseError->protocolType.has_value())
            {
                if (const auto cacheError =
                        journal.restoreRejected(mutation, std::nullopt, *parseError->protocolType))
                    co_return operationError(*cacheError);
            }
            else if (const auto cacheError =
                         journal.transition(mutation, sync::MutationStatus::Unknown))
                co_return operationError(*cacheError);
            co_return *parseError;
        }
        const auto& response = std::get<detail::SetResponse>(parsed);
        auto acceptedScripts = projectedScripts;
        if (!response.updated || !response.updated->contains(script.id))
        {
            if (const auto cacheError = journal.transition(mutation, sync::MutationStatus::Unknown))
                co_return operationError(*cacheError);
            co_return error(OperationErrorCode::ProtocolViolation,
                            QStringLiteral("The server did not confirm the script state change."));
        }
        if (response.updated)
        {
            for (const auto& [id, patch] : *response.updated)
            {
                const auto found = std::ranges::find(acceptedScripts, id, &SieveScript::id);
                if (found != acceptedScripts.end() && patch.has_value())
                    found->isActive = patch->isActive;
            }
        }
        const auto changed = std::ranges::find(acceptedScripts, script.id, &SieveScript::id);
        if (changed == acceptedScripts.cend() || changed->isActive != active)
        {
            if (const auto cacheError = journal.transition(mutation, sync::MutationStatus::Unknown))
                co_return operationError(*cacheError);
            co_return error(OperationErrorCode::ProtocolViolation,
                            active ? QStringLiteral("The server did not activate the script.")
                                   : QStringLiteral("The server did not deactivate the script."));
        }
        if (const auto acceptedError = acceptMutation(m_connection, repository, mutation,
                                                      acceptedScripts, response.newState))
            co_return *acceptedError;
        co_return std::monostate{};
    }
} // namespace javelin::jmap::sieve
