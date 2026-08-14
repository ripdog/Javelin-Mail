#include "protocol/ProtocolValidation.h"

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

        [[nodiscard]] std::optional<BoundaryError> optionalStringError(const QString& value,
                                                                       const QString& field,
                                                                       const BoundaryLimits& limits)
        {
            if (isTooLarge(value, limits))
            {
                return BoundaryError{.code = BoundaryErrorCode::ValueTooLarge,
                                     .field = field,
                                     .detail = QStringLiteral("value exceeds the protocol limit")};
            }
            return std::nullopt;
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
                    else
                    {
                        if (static_cast<std::size_t>(command.payload.size()) >
                            limits.maximumFrameBytes)
                        {
                            return BoundaryError{
                                .code = BoundaryErrorCode::ValueTooLarge,
                                .field = QStringLiteral("command.remote.payload"),
                                .detail = QStringLiteral("remote action payload is too large")};
                        }
                        return std::nullopt;
                    }
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
            const auto validateStringList =
                [&limits](const std::vector<QString>& values,
                          const QString& field) -> std::optional<BoundaryError>
            {
                if (values.size() > limits.maximumCollectionItems)
                {
                    return BoundaryError{
                        .code = BoundaryErrorCode::TooManyValues,
                        .field = field,
                        .detail = QStringLiteral("collection exceeds the protocol limit")};
                }
                for (const auto& value : values)
                {
                    if (auto error = requiredStringError(value, field, limits))
                        return error;
                }
                return std::nullopt;
            };

            if (request.update.accounts.has_value())
            {
                if (request.update.accounts->size() > limits.maximumCollectionItems)
                {
                    return BoundaryError{
                        .code = BoundaryErrorCode::TooManyValues,
                        .field = QStringLiteral("update.accounts"),
                        .detail = QStringLiteral("collection exceeds the protocol limit")};
                }
                for (const auto& account : *request.update.accounts)
                {
                    if (auto error = requiredStringError(
                            account.id, QStringLiteral("update.accounts.id"), limits))
                        return error;
                    if (auto error = optionalStringError(
                            account.displayName, QStringLiteral("update.accounts.displayName"),
                            limits))
                        return error;
                    if (auto error = optionalStringError(
                            account.sessionUrl, QStringLiteral("update.accounts.sessionUrl"),
                            limits))
                        return error;
                    if (auto error = optionalStringError(
                            account.loginEmail, QStringLiteral("update.accounts.loginEmail"),
                            limits))
                        return error;
                    if (auto error = optionalStringError(
                            account.tokenEndpoint, QStringLiteral("update.accounts.tokenEndpoint"),
                            limits))
                        return error;
                    if (auto error = optionalStringError(
                            account.oauthClientId, QStringLiteral("update.accounts.oauthClientId"),
                            limits))
                        return error;
                    if (auto error = optionalStringError(
                            account.oauthIssuer, QStringLiteral("update.accounts.oauthIssuer"),
                            limits))
                        return error;
                    if (auto error = optionalStringError(
                            account.oauthResource, QStringLiteral("update.accounts.oauthResource"),
                            limits))
                        return error;
                    if (auto error = optionalStringError(
                            account.oauthScope, QStringLiteral("update.accounts.oauthScope"),
                            limits))
                        return error;
                    if (auto error = optionalStringError(
                            account.revocationEndpoint,
                            QStringLiteral("update.accounts.revocationEndpoint"), limits))
                        return error;
                    if (auto error = optionalStringError(
                            account.registrationClientUri,
                            QStringLiteral("update.accounts.registrationClientUri"), limits))
                        return error;
                    if (auto error = optionalStringError(
                            account.credentialHandle,
                            QStringLiteral("update.accounts.credentialHandle"), limits))
                        return error;
                    if (account.tokenExpiresAtEpochSeconds < 0)
                    {
                        return BoundaryError{
                            .code = BoundaryErrorCode::InvalidRequest,
                            .field = QStringLiteral("update.accounts.tokenExpiresAtEpochSeconds"),
                            .detail = QStringLiteral("token expiry must not be negative"),
                        };
                    }
                    if (auto error =
                            validateStringList(account.cachedAccountIds,
                                               QStringLiteral("update.accounts.cachedAccountIds")))
                        return error;
                }
            }

            const auto validateSelections =
                [&validateStringList, &limits](const std::vector<MailboxSelectionSettings>& values,
                                               const QString& field) -> std::optional<BoundaryError>
            {
                if (values.size() > limits.maximumCollectionItems)
                {
                    return BoundaryError{
                        .code = BoundaryErrorCode::TooManyValues,
                        .field = field,
                        .detail = QStringLiteral("collection exceeds the protocol limit")};
                }
                for (const auto& selection : values)
                {
                    if (auto error = requiredStringError(
                            selection.accountId, field + QStringLiteral(".accountId"), limits))
                        return error;
                    if (auto error = validateStringList(selection.mailboxIds,
                                                        field + QStringLiteral(".mailboxIds")))
                        return error;
                }
                return std::nullopt;
            };

            if (request.update.syncedMailboxSelections.has_value())
            {
                if (auto error =
                        validateSelections(*request.update.syncedMailboxSelections,
                                           QStringLiteral("update.syncedMailboxSelections")))
                    return error;
            }
            if (request.update.notificationMailboxSelections.has_value())
            {
                if (auto error =
                        validateSelections(*request.update.notificationMailboxSelections,
                                           QStringLiteral("update.notificationMailboxSelections")))
                    return error;
            }
            if (request.update.remoteContentSenders.has_value())
            {
                if (auto error = validateStringList(*request.update.remoteContentSenders,
                                                    QStringLiteral("update.remoteContentSenders")))
                    return error;
            }
            if (request.update.remoteContentDomains.has_value())
            {
                if (auto error = validateStringList(*request.update.remoteContentDomains,
                                                    QStringLiteral("update.remoteContentDomains")))
                    return error;
            }
            if (request.update.appearance.has_value() &&
                (request.update.appearance->messageColorMode < 0 ||
                 request.update.appearance->messageColorMode > 2))
            {
                return BoundaryError{.code = BoundaryErrorCode::InvalidRequest,
                                     .field = QStringLiteral("update.appearance.messageColorMode"),
                                     .detail =
                                         QStringLiteral("color mode is outside the allowed range")};
            }
            if (request.update.attachments.has_value())
            {
                if (auto error =
                        optionalStringError(request.update.attachments->directory,
                                            QStringLiteral("update.attachments.directory"), limits))
                    return error;
            }
            if (request.update.undoSendDelaySeconds.has_value() &&
                (*request.update.undoSendDelaySeconds < 1 ||
                 *request.update.undoSendDelaySeconds > 120))
            {
                return BoundaryError{.code = BoundaryErrorCode::InvalidRequest,
                                     .field = QStringLiteral("update.undoSendDelaySeconds"),
                                     .detail =
                                         QStringLiteral("delay is outside the allowed range")};
            }
            if (request.update.workspace.has_value())
            {
                const auto& workspace = *request.update.workspace;
                if (workspace.formatVersion != 1)
                {
                    return BoundaryError{.code = BoundaryErrorCode::InvalidRequest,
                                         .field = QStringLiteral("update.workspace.formatVersion"),
                                         .detail =
                                             QStringLiteral("workspace format is unsupported")};
                }
                if (static_cast<std::size_t>(workspace.mainWindowState.size()) >
                    limits.maximumWorkspaceBytes)
                {
                    return BoundaryError{
                        .code = BoundaryErrorCode::ValueTooLarge,
                        .field = QStringLiteral("update.workspace.mainWindowState"),
                        .detail = QStringLiteral("workspace state exceeds the protocol limit")};
                }
                if (workspace.calendarColorOverrides.size() > limits.maximumCollectionItems)
                {
                    return BoundaryError{
                        .code = BoundaryErrorCode::TooManyValues,
                        .field = QStringLiteral("update.workspace.calendarColorOverrides"),
                        .detail = QStringLiteral("collection exceeds the protocol limit")};
                }
                for (const auto& overrideValue : workspace.calendarColorOverrides)
                {
                    if (auto error = requiredStringError(
                            overrideValue.calendarId,
                            QStringLiteral("update.workspace.calendarColorOverrides.calendarId"),
                            limits))
                        return error;
                    if (auto error = requiredStringError(
                            overrideValue.color,
                            QStringLiteral("update.workspace.calendarColorOverrides.color"),
                            limits))
                        return error;
                }
                if (workspace.emailContextMenuLayout.size() > limits.maximumCollectionItems)
                {
                    return BoundaryError{
                        .code = BoundaryErrorCode::TooManyValues,
                        .field = QStringLiteral("update.workspace.emailContextMenuLayout"),
                        .detail = QStringLiteral("collection exceeds the protocol limit")};
                }
                for (const auto& actionId : workspace.emailContextMenuLayout)
                {
                    if (auto error = requiredStringError(
                            actionId,
                            QStringLiteral("update.workspace.emailContextMenuLayout.actionId"),
                            limits))
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
                                else
                                    return sizeof(command.action.value) +
                                           static_cast<std::size_t>(command.payload.size());
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
                    if (value.update.accounts.has_value())
                    {
                        for (const auto& account : *value.update.accounts)
                        {
                            size +=
                                stringSize(account.id) + stringSize(account.displayName) +
                                stringSize(account.sessionUrl) + stringSize(account.loginEmail) +
                                stringSize(account.tokenEndpoint) +
                                stringSize(account.oauthClientId) +
                                stringSize(account.oauthIssuer) +
                                stringSize(account.oauthResource) + stringSize(account.oauthScope) +
                                stringSize(account.revocationEndpoint) +
                                stringSize(account.registrationClientUri) +
                                stringSize(account.credentialHandle) + 2;
                            for (const auto& accountId : account.cachedAccountIds)
                                size += stringSize(accountId);
                        }
                    }
                    if (value.update.syncedMailboxSelections.has_value())
                    {
                        for (const auto& selection : *value.update.syncedMailboxSelections)
                        {
                            size += stringSize(selection.accountId);
                            for (const auto& mailboxId : selection.mailboxIds)
                                size += stringSize(mailboxId);
                        }
                    }
                    if (value.update.notificationMailboxSelections.has_value())
                    {
                        for (const auto& selection : *value.update.notificationMailboxSelections)
                        {
                            size += stringSize(selection.accountId);
                            for (const auto& mailboxId : selection.mailboxIds)
                                size += stringSize(mailboxId);
                        }
                    }
                    const auto addStrings =
                        [&size](const std::optional<std::vector<QString>>& values)
                    {
                        if (!values.has_value())
                            return;
                        for (const auto& item : *values)
                            size += stringSize(item);
                    };
                    addStrings(value.update.remoteContentSenders);
                    addStrings(value.update.remoteContentDomains);
                    if (value.update.attachments.has_value())
                        size += stringSize(value.update.attachments->directory);
                    if (value.update.appearance.has_value())
                        size += sizeof(value.update.appearance->messageColorMode);
                    if (value.update.undoSendDelaySeconds.has_value())
                        size += sizeof(*value.update.undoSendDelaySeconds);
                    if (value.update.workspace.has_value())
                    {
                        size += sizeof(value.update.workspace->formatVersion) +
                                sizeof(value.update.workspace->composeRichTextDefault) +
                                static_cast<std::size_t>(
                                    value.update.workspace->mainWindowState.size());
                        for (const auto& overrideValue :
                             value.update.workspace->calendarColorOverrides)
                            size += stringSize(overrideValue.calendarId) +
                                    stringSize(overrideValue.color);
                        for (const auto& actionId : value.update.workspace->emailContextMenuLayout)
                            size += stringSize(actionId);
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
                    std::size_t size =
                        48 + value.changedDomains.size() + stringSize(value.accountId);
                    for (const auto& key : value.affectedKeys)
                        size += stringSize(key);
                    for (const auto& mailboxId : value.mailboxIds)
                        size += stringSize(mailboxId);
                    for (const auto& window : value.mailboxWindows)
                        size += 32 + stringSize(window.mailboxId);
                    for (const auto& window : value.searchWindows)
                        size += 32 + stringSize(window.queryKey);
                    return size;
                }
                else if constexpr (std::is_same_v<Event, OperationFailed>)
                    return 48 + stringSize(value.error.field) + stringSize(value.error.detail);
                else if constexpr (std::is_same_v<Event, OperationCompleted>)
                    return 32 + static_cast<std::size_t>(value.result.size());
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
                else if constexpr (std::is_same_v<Event, DaemonLogEntries>)
                {
                    std::size_t size = 16;
                    for (const auto& entry : value.entries)
                        size += 16 + stringSize(entry.subsystem) + stringSize(entry.message);
                    return size;
                }
                else if constexpr (std::is_same_v<Event, ThreadMaterializationProgress>)
                {
                    std::size_t size = 24 + stringSize(value.accountId) + stringSize(value.error);
                    for (const auto& threadId : value.threadIds)
                        size += stringSize(threadId);
                    return size;
                }
                else
                    return 48;
            },
            event);
    }
} // namespace javelin::protocol
