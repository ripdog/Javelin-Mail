#include "protocol/ProcessBoundary.h"

#include <type_traits>

namespace javelin::protocol
{

    namespace
    {

        [[nodiscard]] bool isEmpty(const QString& value)
        {
            return value.isEmpty();
        }

        [[nodiscard]] bool isTooLarge(const QString& value, const BoundaryLimits& limits)
        {
            return static_cast<std::size_t>(value.toUtf8().size()) > limits.maximumStringBytes;
        }

        [[nodiscard]] bool isInvalid(const ProtocolVersion& version)
        {
            return version.major == 0;
        }

        [[nodiscard]] std::optional<BoundaryError> requiredStringError(const QString& value,
                                                                       const QString& field,
                                                                       const BoundaryLimits& limits)
        {
            if (isEmpty(value))
            {
                return BoundaryError{.code = BoundaryErrorCode::InvalidRequest,
                                     .field = field,
                                     .detail = QStringLiteral("value is required")};
            }
            if (isTooLarge(value, limits))
            {
                return BoundaryError{.code = BoundaryErrorCode::ValueTooLarge,
                                     .field = field,
                                     .detail = QStringLiteral("value exceeds the protocol limit")};
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<BoundaryError>
        optionalStringError(const std::optional<QString>& value, const QString& field,
                            const BoundaryLimits& limits)
        {
            if (!value.has_value())
                return std::nullopt;
            return requiredStringError(*value, field, limits);
        }

        [[nodiscard]] std::optional<BoundaryError> identifierError(const QUuid& value,
                                                                   const QString& field)
        {
            if (value.isNull())
            {
                return BoundaryError{.code = BoundaryErrorCode::InvalidIdentifier,
                                     .field = field,
                                     .detail = QStringLiteral("identifier must not be null")};
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<BoundaryError>
        validateRefreshAccount(const RefreshAccountCommand& command, const BoundaryLimits& limits)
        {
            return requiredStringError(command.accountId, QStringLiteral("command.accountId"),
                                       limits);
        }

        [[nodiscard]] std::optional<BoundaryError> validateCommand(const CommandRequest& request,
                                                                   const BoundaryLimits& limits)
        {
            if (const auto error = identifierError(request.id.value, QStringLiteral("command.id")))
                return error;

            return std::visit(
                [&limits](const auto& command) -> std::optional<BoundaryError>
                {
                    using Command = std::decay_t<decltype(command)>;
                    if constexpr (std::is_same_v<Command, RefreshAccountCommand>)
                        return validateRefreshAccount(command, limits);
                },
                request.command);
        }

        [[nodiscard]] std::optional<BoundaryError>
        validateMaterialization(const MaterializationRequest& request, const BoundaryLimits& limits)
        {
            if (const auto error = identifierError(request.id.value, QStringLiteral("request.id")))
                return error;
            if (const auto error =
                    identifierError(request.scope.value, QStringLiteral("request.scope")))
                return error;

            return std::visit(
                [&limits](const auto& materialization) -> std::optional<BoundaryError>
                {
                    using Request = std::decay_t<decltype(materialization)>;
                    if constexpr (std::is_same_v<Request, MailboxWindowMaterialization>)
                    {
                        if (auto error =
                                requiredStringError(materialization.accountId,
                                                    QStringLiteral("request.accountId"), limits))
                            return error;
                        if (auto error =
                                requiredStringError(materialization.mailboxId,
                                                    QStringLiteral("request.mailboxId"), limits))
                            return error;
                        if (materialization.limit == 0 ||
                            materialization.limit > limits.maximumMaterializationItems)
                        {
                            return BoundaryError{
                                .code = BoundaryErrorCode::InvalidRequest,
                                .field = QStringLiteral("request.limit"),
                                .detail = QStringLiteral("limit is outside the allowed range")};
                        }
                    }
                    return std::nullopt;
                },
                request.request);
        }

        [[nodiscard]] std::optional<BoundaryError>
        validateSettingsUpdate(const UpdateSettingsRequest& request, const BoundaryLimits& limits)
        {
            if (const auto error = optionalStringError(
                    request.update.languageTag, QStringLiteral("update.languageTag"), limits))
                return error;

            if (request.update.watchedMailboxIds.has_value())
            {
                if (request.update.watchedMailboxIds->size() > limits.maximumCollectionItems)
                {
                    return BoundaryError{
                        .code = BoundaryErrorCode::TooManyValues,
                        .field = QStringLiteral("update.watchedMailboxIds"),
                        .detail = QStringLiteral("collection exceeds the protocol limit")};
                }
                for (const auto& mailboxId : *request.update.watchedMailboxIds)
                {
                    if (auto error = requiredStringError(
                            mailboxId, QStringLiteral("update.watchedMailboxIds"), limits))
                        return error;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<BoundaryError> validateRequest(const HelloRequest& request,
                                                                   const BoundaryLimits& limits)
        {
            if (isInvalid(request.protocol))
            {
                return BoundaryError{.code = BoundaryErrorCode::InvalidProtocol,
                                     .field = QStringLiteral("protocol.major"),
                                     .detail =
                                         QStringLiteral("protocol major version must be non-zero")};
            }
            if (auto error = requiredStringError(request.build.application,
                                                 QStringLiteral("build.application"), limits))
                return error;
            return requiredStringError(request.build.revision, QStringLiteral("build.revision"),
                                       limits);
        }

        [[nodiscard]] std::optional<BoundaryError>
        validateRequest(const CancelMaterializationScopeRequest& request, const BoundaryLimits&)
        {
            return identifierError(request.scope.value, QStringLiteral("scope"));
        }

        [[nodiscard]] std::optional<BoundaryError>
        validateRequest(const CacheAccessSuspendedAcknowledgement& request, const BoundaryLimits&)
        {
            return identifierError(request.instance.value, QStringLiteral("cache.instance"));
        }

        [[nodiscard]] std::optional<BoundaryError> validateRequest(const GetSettingsRequest&,
                                                                   const BoundaryLimits&)
        {
            return std::nullopt;
        }

        [[nodiscard]] std::optional<BoundaryError> validateRequest(const PingRequest&,
                                                                   const BoundaryLimits&)
        {
            return std::nullopt;
        }

        [[nodiscard]] std::size_t stringSize(const QString& value)
        {
            return static_cast<std::size_t>(value.toUtf8().size());
        }

        [[nodiscard]] std::size_t commandSize(const CommandRequest& request)
        {
            return 64 + std::visit(
                            [](const auto& command)
                            {
                                using Command = std::decay_t<decltype(command)>;
                                if constexpr (std::is_same_v<Command, RefreshAccountCommand>)
                                    return stringSize(command.accountId) + 1;
                            },
                            request.command);
        }

        [[nodiscard]] std::size_t materializationSize(const MaterializationRequest& request)
        {
            return 80 + std::visit(
                            [](const auto& materialization)
                            {
                                using Request = std::decay_t<decltype(materialization)>;
                                if constexpr (std::is_same_v<Request, MailboxWindowMaterialization>)
                                    return stringSize(materialization.accountId) +
                                           stringSize(materialization.mailboxId) + 12;
                            },
                            request.request);
        }

    } // namespace

    std::optional<BoundaryError> validate(const ClientRequest& request,
                                          const BoundaryLimits& limits)
    {
        return std::visit(
            [&limits](const auto& value) -> std::optional<BoundaryError>
            {
                using Request = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Request, CommandRequest>)
                    return validateCommand(value, limits);
                else if constexpr (std::is_same_v<Request, MaterializationRequest>)
                    return validateMaterialization(value, limits);
                else if constexpr (std::is_same_v<Request, UpdateSettingsRequest>)
                    return validateSettingsUpdate(value, limits);
                else
                    return validateRequest(value, limits);
            },
            request);
    }

    std::size_t estimatedEncodedSize(const ClientRequest& request)
    {
        return std::visit(
            [](const auto& value) -> std::size_t
            {
                using Request = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Request, HelloRequest>)
                    return 32 + stringSize(value.build.application) +
                           stringSize(value.build.revision);
                else if constexpr (std::is_same_v<Request, CommandRequest>)
                    return commandSize(value);
                else if constexpr (std::is_same_v<Request, MaterializationRequest>)
                    return materializationSize(value);
                else if constexpr (std::is_same_v<Request, UpdateSettingsRequest>)
                {
                    std::size_t size = 24;
                    if (value.update.languageTag.has_value())
                        size += stringSize(*value.update.languageTag);
                    if (value.update.watchedMailboxIds.has_value())
                    {
                        for (const auto& mailboxId : *value.update.watchedMailboxIds)
                            size += stringSize(mailboxId);
                    }
                    return size;
                }
                else if constexpr (std::is_same_v<Request, CancelMaterializationScopeRequest> ||
                                   std::is_same_v<Request, CacheAccessSuspendedAcknowledgement>)
                    return 32;
                else
                    return 8;
            },
            request);
    }

    std::size_t estimatedEncodedSize(const BoundaryEvent& event)
    {
        return std::visit(
            [](const auto& value) -> std::size_t
            {
                using Event = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Event, CacheInvalidation>)
                {
                    std::size_t size = 24 + value.changedDomains.size();
                    for (const auto& key : value.affectedKeys)
                        size += stringSize(key);
                    return size;
                }
                else if constexpr (std::is_same_v<Event, OperationFailed>)
                    return 48 + stringSize(value.error.field) + stringSize(value.error.detail);
                else if constexpr (std::is_same_v<Event, SettingsUpdated>)
                    return 16;
                else if constexpr (std::is_same_v<Event, ActivationRequested>)
                    return 32;
                else if constexpr (std::is_same_v<Event, DaemonStatusChanged>)
                {
                    std::size_t size = 16;
                    for (const auto& account : value.status.accounts)
                        size += stringSize(account.accountId) + stringSize(account.detail);
                    return size;
                }
                else if constexpr (std::is_same_v<Event, CacheAccessSuspendRequested>)
                    return 32;
                else
                    return 48;
            },
            event);
    }

} // namespace javelin::protocol
