#include "jmap/sieve/SieveService.h"

#include "jmap/api/MethodCaller.h"
#include "jmap/api/RequestBuilder.h"
#include "jmap/api/SessionClient.h"
#include "jmap/api/Transport.h"

#include <glaze/glaze.hpp>

#include <QUrl>

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
    static constexpr auto value =
        glz::object("accountId", &T::accountId, "create", &T::create, "update", &T::update,
                    "destroy", &T::destroy, "onSuccessActivateScript", &T::onSuccessActivateScript,
                    "onSuccessDeactivateScript", &T::onSuccessDeactivateScript);
};

template <> struct glz::meta<javelin::jmap::sieve::detail::SetResponse>
{
    using T = javelin::jmap::sieve::detail::SetResponse;
    static constexpr auto value =
        glz::object("accountId", &T::accountId, "oldState", &T::oldState, "newState", &T::newState,
                    "notUpdated", &T::notUpdated, "created", &T::created, "notCreated",
                    &T::notCreated, "notDestroyed", &T::notDestroyed);
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
    } // namespace

    SieveService::SieveService(api::AbstractTransport& resourceTransport,
                               api::JmapMethodTransport& methodTransport)
        : m_resourceTransport(resourceTransport), m_methodTransport(methodTransport)
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
        co_return std::move(std::get<detail::GetResponse>(parsed).list);
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

    QCoro::Task<SieveSaveResult> SieveService::save(LiveConnectionSettings settings,
                                                    std::string ownerAccountId, SieveScript script,
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
        const auto& blobId = std::get<std::string>(uploaded);
        auto validation = co_await validateBlob(m_methodTransport, context, blobId);
        if (const auto* validationError = std::get_if<OperationError>(&validation))
            co_return *validationError;
        const auto& validationValue = std::get<SieveValidation>(validation);
        if (!validationValue.valid)
            co_return error(OperationErrorCode::InvalidUserInput, validationValue.message);

        constexpr std::string_view creationId = "new-script";
        const auto arguments = serialize(detail::SetRequest{
            .accountId = context.sieveAccountId,
            .create = script.id.empty()
                          ? std::optional<std::unordered_map<
                                std::string, detail::ScriptCreate>>{{{std::string{creationId},
                                                                      {.name = script.name,
                                                                       .blobId = blobId}}}}
                          : std::nullopt,
            .update =
                script.id.empty()
                    ? std::nullopt
                    : std::optional<std::unordered_map<
                          std::string, detail::ScriptUpdate>>{{{script.id, {.blobId = blobId}}}},
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
            co_return *callError;
        auto parsed = parseMethodResponse<detail::SetResponse>(
            std::get<api::ResponseEnvelope>(called), "sieve-save", sieveSetMethod);
        if (const auto* parseError = std::get_if<OperationError>(&parsed))
            co_return *parseError;
        const auto& response = std::get<detail::SetResponse>(parsed);
        if (script.id.empty())
        {
            if (response.notCreated)
            {
                const auto rejected = response.notCreated->find(std::string{creationId});
                if (rejected != response.notCreated->end())
                    co_return error(rejected->second.type == "invalidSieve"
                                        ? OperationErrorCode::InvalidUserInput
                                        : OperationErrorCode::ProtocolViolation,
                                    rejected->second.description
                                        ? QString::fromStdString(*rejected->second.description)
                                        : QStringLiteral("The server rejected the new script."));
            }
            if (!response.created)
                co_return error(OperationErrorCode::ProtocolViolation,
                                QStringLiteral("The server did not return the new script."));
            const auto created = response.created->find(std::string{creationId});
            if (created == response.created->end())
                co_return error(OperationErrorCode::ProtocolViolation,
                                QStringLiteral("The server did not return the new script."));
            auto createdScript = created->second;
            if (createdScript.name.empty())
                createdScript.name = script.name;
            if (createdScript.blobId.empty())
                createdScript.blobId = blobId;
            co_return createdScript;
        }
        if (response.notUpdated)
        {
            const auto rejected = response.notUpdated->find(script.id);
            if (rejected != response.notUpdated->end())
                co_return error(rejected->second.type == "invalidSieve"
                                    ? OperationErrorCode::InvalidUserInput
                                    : OperationErrorCode::ProtocolViolation,
                                rejected->second.description
                                    ? QString::fromStdString(*rejected->second.description)
                                    : QStringLiteral("The server rejected the script update."));
        }
        script.blobId = blobId;
        co_return script;
    }

    QCoro::Task<SieveDeleteResult> SieveService::remove(LiveConnectionSettings settings,
                                                        std::string ownerAccountId,
                                                        SieveScript script) const
    {
        if (script.id.empty())
            co_return std::monostate{};
        auto contextResult = co_await resolveContext(m_resourceTransport, std::move(settings),
                                                     std::move(ownerAccountId));
        if (const auto* contextError = std::get_if<OperationError>(&contextResult))
            co_return *contextError;
        const auto& context = std::get<detail::ResolvedContext>(contextResult);

        if (script.isActive)
        {
            const auto deactivateArguments = serialize(detail::SetRequest{
                .accountId = context.sieveAccountId,
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
                co_return *callError;
            auto parsed = parseMethodResponse<detail::SetResponse>(
                std::get<api::ResponseEnvelope>(deactivated), "sieve-deactivate", sieveSetMethod);
            if (const auto* parseError = std::get_if<OperationError>(&parsed))
                co_return *parseError;
        }

        const auto destroyArguments = serialize(detail::SetRequest{
            .accountId = context.sieveAccountId,
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
            co_return *callError;
        auto parsed = parseMethodResponse<detail::SetResponse>(
            std::get<api::ResponseEnvelope>(destroyed), "sieve-delete", sieveSetMethod);
        if (const auto* parseError = std::get_if<OperationError>(&parsed))
            co_return *parseError;
        const auto& response = std::get<detail::SetResponse>(parsed);
        if (response.notDestroyed)
        {
            const auto rejected = response.notDestroyed->find(script.id);
            if (rejected != response.notDestroyed->end())
                co_return error(OperationErrorCode::ProtocolViolation,
                                rejected->second.description
                                    ? QString::fromStdString(*rejected->second.description)
                                    : QStringLiteral("The server rejected the script deletion."));
        }
        co_return std::monostate{};
    }

    QCoro::Task<SieveActivationResult> SieveService::setActive(LiveConnectionSettings settings,
                                                               std::string ownerAccountId,
                                                               SieveScript script,
                                                               const bool active) const
    {
        if (script.id.empty())
            co_return error(OperationErrorCode::ProtocolViolation,
                            QStringLiteral("Save the script before activating it."));
        auto contextResult = co_await resolveContext(m_resourceTransport, std::move(settings),
                                                     std::move(ownerAccountId));
        if (const auto* contextError = std::get_if<OperationError>(&contextResult))
            co_return *contextError;
        const auto& context = std::get<detail::ResolvedContext>(contextResult);
        const auto arguments = serialize(detail::SetRequest{
            .accountId = context.sieveAccountId,
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
            co_return *callError;
        auto parsed = parseMethodResponse<detail::SetResponse>(
            std::get<api::ResponseEnvelope>(called), "sieve-active", sieveSetMethod);
        if (const auto* parseError = std::get_if<OperationError>(&parsed))
            co_return *parseError;
        const auto getArguments = serialize(detail::GetRequest{
            .accountId = context.sieveAccountId,
            .ids = std::vector<std::string>{script.id},
        });
        if (!getArguments)
            co_return error(OperationErrorCode::ProtocolViolation,
                            QStringLiteral("Failed to encode the script state request."));
        auto checked = co_await call(m_methodTransport, context, std::string{sieveGetMethod},
                                     *getArguments, "sieve-active-check");
        if (const auto* callError = std::get_if<OperationError>(&checked))
            co_return *callError;
        auto getResponse = parseMethodResponse<detail::GetResponse>(
            std::get<api::ResponseEnvelope>(checked), "sieve-active-check", sieveGetMethod);
        if (const auto* parseError = std::get_if<OperationError>(&getResponse))
            co_return *parseError;
        const auto& scripts = std::get<detail::GetResponse>(getResponse).list;
        const auto changed = std::ranges::find(scripts, script.id, &SieveScript::id);
        if (changed == scripts.cend() || changed->isActive != active)
            co_return error(OperationErrorCode::ProtocolViolation,
                            active ? QStringLiteral("The server did not activate the script.")
                                   : QStringLiteral("The server did not deactivate the script."));
        co_return std::monostate{};
    }
} // namespace javelin::jmap::sieve
