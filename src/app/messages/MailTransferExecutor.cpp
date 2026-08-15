#include "app/MailTransferExecutor.h"

#include "app/AccountConnectionProvider.h"
#include "app/AccountConnectionSettings.h"
#include "jmap/MessageContentClient.h"
#include "jmap/api/BlobUpload.h"
#include "jmap/api/MailMethods.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/RequestBuilder.h"
#include "jmap/api/ResponseReader.h"
#include "jmap/api/Session.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/MailVault.h"
#include "jmap/cache/RawMessageSourceRepository.h"
#include "jmap/cache/SessionRepository.h"

#include <KLocalizedString>

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace javelin::app
{
    namespace
    {
        using DatabaseError = javelin::jmap::cache::DatabaseError;
        using OperationError = javelin::jmap::OperationError;
        using OperationErrorCode = javelin::jmap::OperationErrorCode;

        [[nodiscard]] OperationError stateError(QString message)
        {
            return {.code = OperationErrorCode::PreconditionFailed, .message = std::move(message)};
        }

        [[nodiscard]] OperationError conflictError(QString message)
        {
            return {.code = OperationErrorCode::Conflict, .message = std::move(message)};
        }

        [[nodiscard]] javelin::jmap::LiveConnectionSettings
        liveSettings(const AccountConnectionSettings& settings)
        {
            return {
                .sessionUrl = settings.sessionUrl,
                .loginEmail = settings.loginEmail,
                .apiKey = settings.apiKey,
            };
        }

        [[nodiscard]] std::variant<javelin::jmap::cache::CachedAccount, OperationError>
        requireAccount(javelin::jmap::cache::AccountRepository& accounts,
                       const std::string& accountId)
        {
            const auto result = accounts.findById(accountId);
            if (const auto* error = std::get_if<DatabaseError>(&result))
                return javelin::jmap::operationError(*error);
            const auto& account =
                std::get<std::optional<javelin::jmap::cache::CachedAccount>>(result);
            if (!account.has_value())
                return stateError(i18n("The mail transfer account is no longer available."));
            if (account->remoteAccountId.empty())
                return stateError(i18n("The mail transfer account has no remote JMAP identity."));
            return *account;
        }

        [[nodiscard]] std::variant<javelin::jmap::api::Session, OperationError>
        requireSession(javelin::jmap::cache::DatabaseConnection& database,
                       const std::string& localAccountId)
        {
            javelin::jmap::cache::SessionRepository sessions{database};
            const auto result = sessions.load(localAccountId);
            if (const auto* error = std::get_if<DatabaseError>(&result))
                return javelin::jmap::operationError(*error);
            const auto& session =
                std::get<std::optional<javelin::jmap::api::Session>>(result);
            if (!session.has_value())
                return stateError(i18n("No cached JMAP session is available for this transfer."));
            return *session;
        }

        [[nodiscard]] javelin::jmap::api::ApiRequestContext
        requestContext(const AccountConnectionSettings& settings,
                       const std::string& localAccountId,
                       const javelin::jmap::api::Session& session)
        {
            return {
                .credentials =
                    {
                        .accountId = localAccountId,
                        .emailAddress = settings.loginEmail,
                        .sessionUrl = settings.sessionUrl,
                        .token = {.accessToken = settings.apiKey,
                                  .refreshToken = std::nullopt,
                                  .expiry = std::nullopt},
                    },
                .apiUrl = session.apiUrl,
                .requestLimits = javelin::jmap::api::coreRequestLimits(session),
            };
        }

        [[nodiscard]] OperationError callerError(const javelin::jmap::api::MethodCallerResult& result)
        {
            if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&result))
                return javelin::jmap::operationError(*error);
            if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&result))
                return javelin::jmap::operationError(*error);
            if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&result))
                return javelin::jmap::operationError(*error);
            return {.code = OperationErrorCode::ProtocolViolation,
                    .message = i18n("The JMAP transfer request returned an invalid result.")};
        }

        [[nodiscard]] MailTransferStatus waitStatus(const OperationError& error)
        {
            if (javelin::jmap::isAuthenticationError(error))
                return MailTransferStatus::WaitingForAuth;
            return MailTransferStatus::WaitingForNetwork;
        }

        [[nodiscard]] bool retryable(const OperationError& error)
        {
            return javelin::jmap::isAuthenticationError(error) ||
                   javelin::jmap::isTransientError(error);
        }

        [[nodiscard]] std::optional<OperationError>
        requireTransition(std::variant<bool, DatabaseError> result, QString failure)
        {
            if (const auto* error = std::get_if<DatabaseError>(&result))
                return javelin::jmap::operationError(*error);
            if (!std::get<bool>(result))
                return conflictError(std::move(failure));
            return std::nullopt;
        }

        [[nodiscard]] std::optional<OperationError>
        releaseSourcePin(MailTransferRepository& repository, const std::string& itemId)
        {
            if (const auto error = repository.releaseSourcePin(itemId))
                return javelin::jmap::operationError(*error);
            return std::nullopt;
        }

        [[nodiscard]] std::unordered_map<std::string, bool>
        keywordMap(const std::vector<std::string>& keywords)
        {
            std::unordered_map<std::string, bool> result;
            result.reserve(keywords.size());
            for (const auto& keyword : keywords)
                result.emplace(keyword, true);
            return result;
        }

        [[nodiscard]] OperationError sourceUnavailable(const QString& message)
        {
            return {
                .code = OperationErrorCode::NotFound,
                .message = message,
            };
        }
    } // namespace

    MailTransferExecutor::MailTransferExecutor(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::api::AbstractTransport& resourceTransport,
        javelin::jmap::api::JmapMethodTransport& methodTransport,
        javelin::jmap::MessageContentClient& messageContentClient,
        const AccountConnectionProvider& connectionProvider)
        : m_databaseConnection(databaseConnection), m_resourceTransport(resourceTransport),
          m_methodTransport(methodTransport), m_messageContentClient(messageContentClient),
          m_connectionProvider(connectionProvider)
    {
    }

    QCoro::Task<MailTransferExecutionResult>
    MailTransferExecutor::advance(std::string operationId)
    {
        MailTransferRepository repository{m_databaseConnection};
        const auto operationResult = repository.findOperation(operationId);
        if (const auto* error = std::get_if<DatabaseError>(&operationResult))
            co_return javelin::jmap::operationError(*error);
        const auto& maybeOperation =
            std::get<std::optional<MailTransferOperationRecord>>(operationResult);
        if (!maybeOperation.has_value())
            co_return stateError(i18n("The mail transfer no longer exists."));
        auto operation = *maybeOperation;
        if (operation.topology != MailTransferTopology::CrossServerImport)
        {
            co_return OperationError{
                .code = OperationErrorCode::UnsupportedCapability,
                .message = i18n("This executor currently handles cross-server imports only."),
            };
        }

        auto itemsResult = repository.listItems(operation.operationId);
        if (const auto* error = std::get_if<DatabaseError>(&itemsResult))
            co_return javelin::jmap::operationError(*error);
        auto items = std::get<std::vector<MailTransferItemRecord>>(std::move(itemsResult));
        if (items.empty())
            co_return stateError(i18n("The mail transfer has no messages to process."));

        javelin::jmap::cache::AccountRepository accounts{m_databaseConnection};
        auto sourceAccountResult = requireAccount(accounts, operation.sourceAccountId);
        if (const auto* error = std::get_if<OperationError>(&sourceAccountResult))
            co_return *error;
        auto destinationAccountResult = requireAccount(accounts, operation.destinationAccountId);
        if (const auto* error = std::get_if<OperationError>(&destinationAccountResult))
            co_return *error;
        const auto sourceAccount =
            std::get<javelin::jmap::cache::CachedAccount>(std::move(sourceAccountResult));
        const auto destinationAccount =
            std::get<javelin::jmap::cache::CachedAccount>(std::move(destinationAccountResult));
        if (sourceAccount.connectionId == destinationAccount.connectionId)
            co_return stateError(i18n("The transfer topology no longer matches the account state."));

        const auto sourceSettings =
            m_connectionProvider.connectionSettingsFor(operation.sourceAccountId);
        const auto destinationSettings =
            m_connectionProvider.connectionSettingsFor(operation.destinationAccountId);
        if (!sourceSettings.has_value() || !destinationSettings.has_value())
            co_return stateError(i18n("Connection settings are unavailable for this transfer."));

        auto destinationSessionResult =
            requireSession(m_databaseConnection, operation.destinationAccountId);
        if (const auto* error = std::get_if<OperationError>(&destinationSessionResult))
            co_return *error;
        const auto destinationSession =
            std::get<javelin::jmap::api::Session>(std::move(destinationSessionResult));
        if (!destinationSession.accounts.contains(destinationAccount.remoteAccountId))
            co_return stateError(i18n("The destination account is absent from its JMAP session."));
        const auto uploadContextResult = javelin::jmap::api::blobUploadContext(
            destinationSession, destinationAccount.remoteAccountId);
        if (const auto* error =
                std::get_if<javelin::jmap::api::ProtocolError>(&uploadContextResult))
            co_return javelin::jmap::operationError(*error);
        const auto uploadContext =
            std::get<javelin::jmap::api::BlobUploadContext>(uploadContextResult);
        const auto destinationRequestContext =
            requestContext(*destinationSettings, operation.destinationAccountId, destinationSession);
        if (!destinationRequestContext.requestLimits.has_value())
        {
            co_return OperationError{
                .code = OperationErrorCode::UnsupportedCapability,
                .message = i18n("The destination server does not advertise usable JMAP limits."),
            };
        }

        if (const auto error = repository.updateStatus(operation.operationId,
                                                       MailTransferStatus::Running))
            co_return javelin::jmap::operationError(*error);

        javelin::jmap::cache::RawMessageSourceRepository sourceRepository{m_databaseConnection};
        const auto vault = javelin::jmap::cache::MailVault::forDatabase(m_databaseConnection);
        javelin::jmap::api::MethodCaller methodCaller{m_methodTransport};

        for (auto& item : items)
        {
            if (item.phase == MailTransferItemPhase::Complete ||
                item.phase == MailTransferItemPhase::Failed ||
                item.phase == MailTransferItemPhase::Cancelled ||
                item.phase == MailTransferItemPhase::DestinationUnknown ||
                item.phase == MailTransferItemPhase::SourceCleanupUnknown ||
                item.phase == MailTransferItemPhase::PartialSourceRetained)
                continue;

            if (item.phase == MailTransferItemPhase::CreatingDestination)
            {
                const QString message = i18n(
                    "Javelin restarted after destination creation may have been dispatched. The "
                    "destination must be reconciled before this transfer can continue.");
                if (const auto error = requireTransition(
                        repository.transitionItem(item.itemId,
                                                  MailTransferItemPhase::CreatingDestination,
                                                  MailTransferItemPhase::DestinationUnknown,
                                                  message),
                        i18n("The destination transfer state changed while recovering.")))
                    co_return *error;
                item.phase = MailTransferItemPhase::DestinationUnknown;
                item.lastError = message;
                continue;
            }

            if (item.phase == MailTransferItemPhase::Prepared)
            {
                if (const auto error = requireTransition(
                        repository.transitionItem(item.itemId, MailTransferItemPhase::Prepared,
                                                  MailTransferItemPhase::AcquiringSource),
                        i18n("The source transfer state changed before acquisition.")))
                    co_return *error;
                item.phase = MailTransferItemPhase::AcquiringSource;
            }

            if (item.phase == MailTransferItemPhase::AcquiringSource)
            {
                auto refreshed = co_await m_messageContentClient.refresh(
                    liveSettings(*sourceSettings), operation.sourceAccountId, item.sourceEmailId);
                if (const auto* error = std::get_if<OperationError>(&refreshed))
                {
                    if (retryable(*error))
                    {
                        if (const auto databaseError = repository.updateStatus(
                                operation.operationId, waitStatus(*error), error->message))
                            co_return javelin::jmap::operationError(*databaseError);
                        co_return *error;
                    }
                    if (const auto transitionError = requireTransition(
                            repository.transitionItem(item.itemId,
                                                      MailTransferItemPhase::AcquiringSource,
                                                      MailTransferItemPhase::Failed,
                                                      error->message),
                            i18n("The source transfer state changed while failing acquisition.")))
                        co_return *transitionError;
                    item.phase = MailTransferItemPhase::Failed;
                    item.lastError = error->message;
                    continue;
                }
                if (const auto* unavailable =
                        std::get_if<javelin::jmap::MessageContentUnavailable>(&refreshed))
                {
                    const auto error = sourceUnavailable(unavailable->message);
                    if (const auto transitionError = requireTransition(
                            repository.transitionItem(item.itemId,
                                                      MailTransferItemPhase::AcquiringSource,
                                                      MailTransferItemPhase::Failed,
                                                      unavailable->message),
                            i18n("The source transfer state changed while failing acquisition.")))
                        co_return *transitionError;
                    item.phase = MailTransferItemPhase::Failed;
                    item.lastError = error.message;
                    continue;
                }

                auto referenceResult =
                    sourceRepository.findReference(operation.sourceAccountId, item.sourceEmailId);
                if (const auto* error = std::get_if<DatabaseError>(&referenceResult))
                    co_return javelin::jmap::operationError(*error);
                auto reference = std::get<std::optional<javelin::jmap::cache::RawMessageSourceReference>>(
                    std::move(referenceResult));
                if (!reference.has_value())
                {
                    const auto migration = sourceRepository.migrateLegacySources(25);
                    if (const auto* error = std::get_if<DatabaseError>(&migration))
                        co_return javelin::jmap::operationError(*error);
                    referenceResult =
                        sourceRepository.findReference(operation.sourceAccountId, item.sourceEmailId);
                    if (const auto* error = std::get_if<DatabaseError>(&referenceResult))
                        co_return javelin::jmap::operationError(*error);
                    reference = std::get<
                        std::optional<javelin::jmap::cache::RawMessageSourceReference>>(
                        std::move(referenceResult));
                }
                if (!reference.has_value() || reference->blobId != item.sourceBlobId)
                {
                    const QString message = i18n(
                        "The exact raw source captured for this transfer is no longer available.");
                    if (const auto error = requireTransition(
                            repository.transitionItem(item.itemId,
                                                      MailTransferItemPhase::AcquiringSource,
                                                      MailTransferItemPhase::Failed, message),
                            i18n("The source transfer state changed while validating content.")))
                        co_return *error;
                    item.phase = MailTransferItemPhase::Failed;
                    item.lastError = message;
                    continue;
                }
                if (const auto error = requireTransition(
                        repository.markSourceReady(item.itemId,
                                                   MailTransferItemPhase::AcquiringSource,
                                                   reference->object.contentHash),
                        i18n("The source transfer state changed while pinning content.")))
                    co_return *error;
                item.rawContentHash = reference->object.contentHash;
                item.phase = MailTransferItemPhase::SourceReady;
            }

            if (item.phase == MailTransferItemPhase::SourceReady)
            {
                if (const auto error = requireTransition(
                        repository.transitionItem(item.itemId, MailTransferItemPhase::SourceReady,
                                                  MailTransferItemPhase::Uploading),
                        i18n("The transfer state changed before upload.")))
                    co_return *error;
                item.phase = MailTransferItemPhase::Uploading;
            }

            if (item.phase == MailTransferItemPhase::Uploading)
            {
                if (!item.rawContentHash.has_value())
                    co_return stateError(i18n("The transfer is missing its pinned raw source."));
                const auto objectResult = sourceRepository.findVaultObject(*item.rawContentHash);
                if (const auto* error = std::get_if<DatabaseError>(&objectResult))
                    co_return javelin::jmap::operationError(*error);
                const auto& object =
                    std::get<std::optional<javelin::jmap::cache::MailVaultObject>>(objectResult);
                if (!object.has_value())
                    co_return stateError(i18n("The pinned raw source is missing from the mail vault."));
                auto leaseResult = vault.acquireLease(*object);
                if (const auto* error =
                        std::get_if<javelin::jmap::cache::MailVaultError>(&leaseResult))
                    co_return OperationError{.code = OperationErrorCode::LocalStorageFailure,
                                             .message = error->message};
                auto lease = std::get<javelin::jmap::cache::MailVaultLease>(std::move(leaseResult));
                const auto pathResult = lease.filePath();
                if (const auto* error =
                        std::get_if<javelin::jmap::cache::MailVaultError>(&pathResult))
                    co_return OperationError{.code = OperationErrorCode::LocalStorageFailure,
                                             .message = error->message};

                auto upload = co_await javelin::jmap::api::uploadBlobFromFile(
                    m_resourceTransport, uploadContext, operation.destinationAccountId,
                    destinationAccount.remoteAccountId, destinationSettings->apiKey,
                    std::get<QString>(pathResult), "message/rfc822");
                if (const auto* error =
                        std::get_if<javelin::jmap::api::TransportError>(&upload))
                {
                    const auto operationError = javelin::jmap::operationError(*error);
                    if (retryable(operationError))
                    {
                        if (const auto databaseError = repository.updateStatus(
                                operation.operationId, waitStatus(operationError),
                                operationError.message))
                            co_return javelin::jmap::operationError(*databaseError);
                        co_return operationError;
                    }
                    if (const auto transitionError = requireTransition(
                            repository.transitionItem(item.itemId, MailTransferItemPhase::Uploading,
                                                      MailTransferItemPhase::Failed,
                                                      operationError.message),
                            i18n("The transfer state changed while failing upload.")))
                        co_return *transitionError;
                    if (const auto pinError = releaseSourcePin(repository, item.itemId))
                        co_return *pinError;
                    item.phase = MailTransferItemPhase::Failed;
                    item.lastError = operationError.message;
                    continue;
                }
                if (const auto* error =
                        std::get_if<javelin::jmap::api::ProtocolError>(&upload))
                {
                    const auto operationError = javelin::jmap::operationError(*error);
                    if (const auto transitionError = requireTransition(
                            repository.transitionItem(item.itemId, MailTransferItemPhase::Uploading,
                                                      MailTransferItemPhase::Failed,
                                                      operationError.message),
                            i18n("The transfer state changed while failing upload.")))
                        co_return *transitionError;
                    if (const auto pinError = releaseSourcePin(repository, item.itemId))
                        co_return *pinError;
                    item.phase = MailTransferItemPhase::Failed;
                    item.lastError = operationError.message;
                    continue;
                }
                const auto& uploaded =
                    std::get<javelin::jmap::api::BlobUploadResponse>(upload);
                if (const auto error = requireTransition(
                        repository.markUploaded(item.itemId, MailTransferItemPhase::Uploading,
                                                uploaded.blobId),
                        i18n("The transfer state changed while recording the uploaded blob.")))
                    co_return *error;
                item.destinationUploadBlobId = uploaded.blobId;
                item.phase = MailTransferItemPhase::Uploaded;
            }

            if (item.phase == MailTransferItemPhase::Uploaded)
            {
                const auto stateRequest = javelin::jmap::api::emailGet({
                    .accountId = destinationAccount.remoteAccountId,
                    .ids = std::vector<std::string>{},
                    .idsReference = std::nullopt,
                    .properties = std::vector<std::string>{"id"},
                });
                if (!stateRequest.has_value())
                    co_return stateError(i18n("Unable to encode the destination Email/get request."));
                javelin::jmap::api::RequestBuilder stateBuilder;
                stateBuilder.useCore().useMail();
                const auto stateHandle = stateBuilder.call(*stateRequest, "mail-transfer-state");
                auto stateCalled = co_await methodCaller.call(destinationRequestContext, stateBuilder);
                if (!std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(stateCalled))
                {
                    const auto error = callerError(stateCalled);
                    if (retryable(error))
                    {
                        if (const auto databaseError = repository.updateStatus(
                                operation.operationId, waitStatus(error), error.message))
                            co_return javelin::jmap::operationError(*databaseError);
                        co_return error;
                    }
                    if (const auto transitionError = requireTransition(
                            repository.transitionItem(item.itemId, MailTransferItemPhase::Uploaded,
                                                      MailTransferItemPhase::Failed, error.message),
                            i18n("The transfer state changed while failing destination preflight.")))
                        co_return *transitionError;
                    if (const auto pinError = releaseSourcePin(repository, item.itemId))
                        co_return *pinError;
                    item.phase = MailTransferItemPhase::Failed;
                    item.lastError = error.message;
                    continue;
                }
                const auto stateRead = javelin::jmap::api::ResponseReader{
                    std::get<javelin::jmap::api::ResponseEnvelope>(stateCalled)}.require(stateHandle);
                if (const auto* readError =
                        std::get_if<javelin::jmap::api::ResponseReaderError>(&stateRead))
                {
                    const auto error = javelin::jmap::operationError(*readError);
                    if (const auto transitionError = requireTransition(
                            repository.transitionItem(item.itemId, MailTransferItemPhase::Uploaded,
                                                      MailTransferItemPhase::Failed, error.message),
                            i18n("The transfer state changed while failing destination preflight.")))
                        co_return *transitionError;
                    if (const auto pinError = releaseSourcePin(repository, item.itemId))
                        co_return *pinError;
                    item.phase = MailTransferItemPhase::Failed;
                    item.lastError = error.message;
                    continue;
                }
                const auto& stateResponse =
                    std::get<javelin::jmap::api::EmailGetResponse>(stateRead);
                if (stateResponse.accountId != destinationAccount.remoteAccountId ||
                    stateResponse.state.empty())
                {
                    const QString message = i18n("The destination Email state response is invalid.");
                    if (const auto transitionError = requireTransition(
                            repository.transitionItem(item.itemId, MailTransferItemPhase::Uploaded,
                                                      MailTransferItemPhase::Failed, message),
                            i18n("The transfer state changed while failing destination preflight.")))
                        co_return *transitionError;
                    if (const auto pinError = releaseSourcePin(repository, item.itemId))
                        co_return *pinError;
                    item.phase = MailTransferItemPhase::Failed;
                    item.lastError = message;
                    continue;
                }

                if (const auto error = requireTransition(
                        repository.markDestinationDispatching(
                            item.itemId, MailTransferItemPhase::Uploaded, stateResponse.state),
                        i18n("The transfer state changed before destination creation.")))
                    co_return *error;
                item.destinationPreState = stateResponse.state;
                item.phase = MailTransferItemPhase::CreatingDestination;

                if (!item.destinationUploadBlobId.has_value())
                    co_return stateError(i18n("The transfer is missing its destination upload blob."));
                javelin::jmap::api::EmailImport import{
                    .blobId = *item.destinationUploadBlobId,
                    .mailboxIds = {{operation.destinationMailboxId, true}},
                    .keywords = keywordMap(item.sourceKeywords),
                    .receivedAt = item.sourceReceivedAt,
                };
                const auto importRequest = javelin::jmap::api::emailImport({
                    .accountId = destinationAccount.remoteAccountId,
                    .ifInState = stateResponse.state,
                    .emails = {{item.destinationCreationId, std::move(import)}},
                });
                if (!importRequest.has_value())
                    co_return stateError(i18n("Unable to encode the Email/import request."));
                javelin::jmap::api::RequestBuilder importBuilder;
                importBuilder.useCore().useMail();
                const auto importHandle = importBuilder.call(*importRequest, "mail-transfer-import");
                bool dispatched = false;
                auto importCalled = co_await methodCaller.call(
                    destinationRequestContext, importBuilder, {}, [&dispatched] { dispatched = true; });
                if (!std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(importCalled))
                {
                    const auto error = callerError(importCalled);
                    if (dispatched)
                    {
                        if (const auto transitionError = requireTransition(
                                repository.transitionItem(
                                    item.itemId, MailTransferItemPhase::CreatingDestination,
                                    MailTransferItemPhase::DestinationUnknown, error.message),
                                i18n("The transfer state changed while recording an unknown "
                                     "destination outcome.")))
                            co_return *transitionError;
                        item.phase = MailTransferItemPhase::DestinationUnknown;
                        item.lastError = error.message;
                        continue;
                    }
                    if (retryable(error))
                    {
                        if (const auto transitionError = requireTransition(
                                repository.transitionItem(
                                    item.itemId, MailTransferItemPhase::CreatingDestination,
                                    MailTransferItemPhase::Uploaded, error.message),
                                i18n("The transfer state changed after a pre-dispatch failure.")))
                            co_return *transitionError;
                        item.phase = MailTransferItemPhase::Uploaded;
                        if (const auto databaseError = repository.updateStatus(
                                operation.operationId, waitStatus(error), error.message))
                            co_return javelin::jmap::operationError(*databaseError);
                        co_return error;
                    }
                    if (const auto transitionError = requireTransition(
                            repository.transitionItem(
                                item.itemId, MailTransferItemPhase::CreatingDestination,
                                MailTransferItemPhase::Failed, error.message),
                            i18n("The transfer state changed while failing destination creation.")))
                        co_return *transitionError;
                    if (const auto pinError = releaseSourcePin(repository, item.itemId))
                        co_return *pinError;
                    item.phase = MailTransferItemPhase::Failed;
                    item.lastError = error.message;
                    continue;
                }

                const auto importRead = javelin::jmap::api::ResponseReader{
                    std::get<javelin::jmap::api::ResponseEnvelope>(importCalled)}.require(importHandle);
                if (const auto* readError =
                        std::get_if<javelin::jmap::api::ResponseReaderError>(&importRead))
                {
                    const auto error = javelin::jmap::operationError(*readError);
                    if (const auto transitionError = requireTransition(
                            repository.transitionItem(
                                item.itemId, MailTransferItemPhase::CreatingDestination,
                                MailTransferItemPhase::DestinationUnknown, error.message),
                            i18n("The transfer state changed while recording an unknown "
                                 "destination response.")))
                        co_return *transitionError;
                    item.phase = MailTransferItemPhase::DestinationUnknown;
                    item.lastError = error.message;
                    continue;
                }

                const auto& importResponse =
                    std::get<javelin::jmap::api::EmailImportResponse>(importRead);
                if (importResponse.accountId != destinationAccount.remoteAccountId)
                {
                    const QString message = i18n("Email/import returned the wrong account id.");
                    if (const auto transitionError = requireTransition(
                            repository.transitionItem(
                                item.itemId, MailTransferItemPhase::CreatingDestination,
                                MailTransferItemPhase::DestinationUnknown, message),
                            i18n("The transfer state changed while recording an unknown "
                                 "destination response.")))
                        co_return *transitionError;
                    item.phase = MailTransferItemPhase::DestinationUnknown;
                    item.lastError = message;
                    continue;
                }

                const auto created = importResponse.created.find(item.destinationCreationId);
                if (created != importResponse.created.end())
                {
                    if (const auto error = requireTransition(
                            repository.markDestinationConfirmed(
                                item.itemId, MailTransferItemPhase::CreatingDestination,
                                {
                                    .emailId = created->second.id,
                                    .blobId = created->second.blobId,
                                    .threadId = created->second.threadId,
                                    .size = created->second.size,
                                    .reusedExisting = false,
                                    .priorMailboxIds = std::nullopt,
                                }),
                            i18n("The transfer state changed while confirming the destination.")))
                        co_return *error;
                    item.destinationEmailId = created->second.id;
                    item.destinationBlobId = created->second.blobId;
                    item.destinationThreadId = created->second.threadId;
                    item.destinationSize = created->second.size;
                    item.phase = MailTransferItemPhase::DestinationConfirmed;
                }
                else
                {
                    const auto rejected = importResponse.notCreated.find(item.destinationCreationId);
                    if (rejected != importResponse.notCreated.end() &&
                        rejected->second.type == "alreadyExists" &&
                        rejected->second.existingId.has_value())
                    {
                        const QString message = i18n(
                            "The destination already contains an equivalent message (%1). Its "
                            "mailbox membership must be reconciled before this transfer can "
                            "continue.",
                            QString::fromStdString(*rejected->second.existingId));
                        if (const auto transitionError = requireTransition(
                                repository.transitionItem(
                                    item.itemId, MailTransferItemPhase::CreatingDestination,
                                    MailTransferItemPhase::DestinationUnknown, message),
                                i18n("The transfer state changed while recording duplicate "
                                     "destination reconciliation.")))
                            co_return *transitionError;
                        item.phase = MailTransferItemPhase::DestinationUnknown;
                        item.lastError = message;
                        continue;
                    }
                    if (rejected != importResponse.notCreated.end())
                    {
                        const QString message = rejected->second.description.has_value()
                                                    ? QString::fromStdString(
                                                          *rejected->second.description)
                                                    : i18n("The destination rejected Email/import "
                                                           "with error %1.",
                                                           QString::fromStdString(
                                                               rejected->second.type));
                        if (const auto transitionError = requireTransition(
                                repository.transitionItem(
                                    item.itemId, MailTransferItemPhase::CreatingDestination,
                                    MailTransferItemPhase::Failed, message),
                                i18n("The transfer state changed while recording destination "
                                     "rejection.")))
                            co_return *transitionError;
                        if (const auto pinError = releaseSourcePin(repository, item.itemId))
                            co_return *pinError;
                        item.phase = MailTransferItemPhase::Failed;
                        item.lastError = message;
                        continue;
                    }

                    const QString message = i18n(
                        "Email/import did not account for the requested destination creation.");
                    if (const auto transitionError = requireTransition(
                            repository.transitionItem(
                                item.itemId, MailTransferItemPhase::CreatingDestination,
                                MailTransferItemPhase::DestinationUnknown, message),
                            i18n("The transfer state changed while recording an unknown "
                                 "destination response.")))
                        co_return *transitionError;
                    item.phase = MailTransferItemPhase::DestinationUnknown;
                    item.lastError = message;
                    continue;
                }
            }

            if (item.phase == MailTransferItemPhase::DestinationConfirmed &&
                operation.operation == MailTransferOperation::Copy)
            {
                if (const auto error = requireTransition(
                        repository.transitionItem(item.itemId,
                                                  MailTransferItemPhase::DestinationConfirmed,
                                                  MailTransferItemPhase::Complete),
                        i18n("The transfer state changed while completing the copy.")))
                    co_return *error;
                if (const auto error = repository.releaseSourcePin(item.itemId))
                    co_return javelin::jmap::operationError(*error);
                item.phase = MailTransferItemPhase::Complete;
            }
        }

        itemsResult = repository.listItems(operation.operationId);
        if (const auto* error = std::get_if<DatabaseError>(&itemsResult))
            co_return javelin::jmap::operationError(*error);
        items = std::get<std::vector<MailTransferItemRecord>>(std::move(itemsResult));

        MailTransferExecutionSummary summary{.operationId = operation.operationId};
        for (const auto& item : items)
        {
            if (item.phase == MailTransferItemPhase::Complete)
                ++summary.completeItemCount;
            if (item.phase == MailTransferItemPhase::DestinationConfirmed)
                ++summary.destinationConfirmedItemCount;
            if (item.phase == MailTransferItemPhase::Failed)
                ++summary.failedItemCount;
            if (item.phase == MailTransferItemPhase::DestinationUnknown ||
                item.phase == MailTransferItemPhase::SourceCleanupUnknown)
                ++summary.unknownItemCount;
        }

        if (summary.unknownItemCount > 0)
            summary.status = MailTransferStatus::BlockedUnknown;
        else if (summary.failedItemCount == items.size())
            summary.status = MailTransferStatus::Failed;
        else if (summary.failedItemCount > 0)
            summary.status = MailTransferStatus::Partial;
        else if (operation.operation == MailTransferOperation::Copy &&
                 summary.completeItemCount == items.size())
            summary.status = MailTransferStatus::Complete;
        else
            summary.status = MailTransferStatus::Running;

        if (const auto error = repository.updateStatus(operation.operationId, summary.status))
            co_return javelin::jmap::operationError(*error);
        co_return summary;
    }

} // namespace javelin::app
