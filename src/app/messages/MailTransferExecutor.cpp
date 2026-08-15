#include "app/MailTransferExecutor.h"

#include "app/AccountConnectionProvider.h"
#include "app/AccountConnectionSettings.h"
#include "jmap/MessageContentClient.h"
#include "jmap/sync/EmailMutationEngine.h"
#include "jmap/sync/EmailMutationJournal.h"
#include "jmap/api/BlobUpload.h"
#include "jmap/api/MailMethods.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/PatchObject.h"
#include "jmap/api/RequestBuilder.h"
#include "jmap/api/ResponseReader.h"
#include "jmap/api/Session.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailVault.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/RawMessageSourceRepository.h"
#include "jmap/cache/SearchWindowRepository.h"
#include "jmap/cache/SessionRepository.h"

#include <KLocalizedString>

#include <algorithm>
#include <ranges>
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

        [[nodiscard]] const std::vector<std::string>& destinationEmailProperties()
        {
            static const std::vector<std::string> properties{
                "id",         "blobId",        "threadId", "mailboxIds", "keywords",
                "size",       "receivedAt",    "sentAt",   "messageId",  "inReplyTo",
                "references", "hasAttachment", "subject",  "from",       "to",
                "cc",         "bcc",           "replyTo",  "preview",
            };
            return properties;
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

        struct DestinationCreationResponse
        {
            std::unordered_map<std::string, javelin::jmap::api::EmailSetCreated> created;
            std::unordered_map<std::string, javelin::jmap::api::SetError> notCreated;
        };

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
        const bool crossServerImport =
            operation.topology == MailTransferTopology::CrossServerImport;
        const bool sameSessionCopy = operation.topology == MailTransferTopology::SameSessionCopy;

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
        if ((crossServerImport && sourceAccount.connectionId == destinationAccount.connectionId) ||
            (sameSessionCopy && sourceAccount.connectionId != destinationAccount.connectionId))
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
        if (sameSessionCopy && !destinationSession.accounts.contains(sourceAccount.remoteAccountId))
            co_return stateError(i18n("The source account is absent from the shared JMAP session."));
        std::optional<javelin::jmap::api::BlobUploadContext> uploadContext;
        if (crossServerImport)
        {
            const auto uploadContextResult = javelin::jmap::api::blobUploadContext(
                destinationSession, destinationAccount.remoteAccountId);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ProtocolError>(&uploadContextResult))
                co_return javelin::jmap::operationError(*error);
            uploadContext =
                std::get<javelin::jmap::api::BlobUploadContext>(uploadContextResult);
        }
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
        javelin::jmap::EmailMutationEngine sourceMutationEngine{m_databaseConnection,
                                                                m_methodTransport};
        javelin::jmap::sync::EmailMutationJournal sourceMutationJournal{m_databaseConnection};

        for (auto& item : items)
        {
            if (item.phase == MailTransferItemPhase::DestinationUnknown)
            {
                const bool canReconcileExisting =
                    item.reusedExisting && item.destinationEmailId.has_value() &&
                    item.destinationPreState.has_value() &&
                    item.destinationPriorMailboxIds.has_value();
                if (!canReconcileExisting)
                    continue;

                const auto existingRequest = javelin::jmap::api::emailGet({
                    .accountId = destinationAccount.remoteAccountId,
                    .ids = std::vector<std::string>{*item.destinationEmailId},
                    .idsReference = std::nullopt,
                    .properties = std::vector<std::string>{
                        "id", "blobId", "threadId", "mailboxIds", "keywords", "size",
                        "receivedAt"},
                });
                if (!existingRequest.has_value())
                    co_return stateError(
                        i18n("Unable to encode the destination reconciliation Email/get request."));
                javelin::jmap::api::RequestBuilder existingBuilder;
                existingBuilder.useCore().useMail();
                const auto existingHandle =
                    existingBuilder.call(*existingRequest, "mail-transfer-existing-reconcile");
                auto existingCalled =
                    co_await methodCaller.call(destinationRequestContext, existingBuilder);
                if (!std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(existingCalled))
                {
                    const auto error = callerError(existingCalled);
                    if (retryable(error))
                    {
                        if (const auto databaseError = repository.updateStatus(
                                operation.operationId, waitStatus(error), error.message))
                            co_return javelin::jmap::operationError(*databaseError);
                        co_return error;
                    }
                    if (const auto transitionError = requireTransition(
                            repository.transitionItem(
                                item.itemId, MailTransferItemPhase::DestinationUnknown,
                                MailTransferItemPhase::DestinationUnknown, error.message),
                            i18n("The transfer state changed while reconciling the destination.")))
                        co_return *transitionError;
                    item.lastError = error.message;
                    continue;
                }

                const auto existingRead = javelin::jmap::api::ResponseReader{
                    std::get<javelin::jmap::api::ResponseEnvelope>(existingCalled)}
                                              .require(existingHandle);
                if (const auto* readError =
                        std::get_if<javelin::jmap::api::ResponseReaderError>(&existingRead))
                {
                    const auto error = javelin::jmap::operationError(*readError);
                    if (const auto transitionError = requireTransition(
                            repository.transitionItem(
                                item.itemId, MailTransferItemPhase::DestinationUnknown,
                                MailTransferItemPhase::DestinationUnknown, error.message),
                            i18n("The transfer state changed while reconciling the destination.")))
                        co_return *transitionError;
                    item.lastError = error.message;
                    continue;
                }

                const auto& existingResponse =
                    std::get<javelin::jmap::api::EmailGetResponse>(existingRead);
                const bool validExisting =
                    existingResponse.accountId == destinationAccount.remoteAccountId &&
                    !existingResponse.state.empty() && existingResponse.list.size() == 1 &&
                    existingResponse.list.front().id == *item.destinationEmailId &&
                    existingResponse.notFound.empty();
                if (!validExisting)
                {
                    const QString message = i18n(
                        "The existing destination message can no longer be reconciled exactly.");
                    if (const auto transitionError = requireTransition(
                            repository.transitionItem(
                                item.itemId, MailTransferItemPhase::DestinationUnknown,
                                MailTransferItemPhase::DestinationUnknown, message),
                            i18n("The transfer state changed while reconciling the destination.")))
                        co_return *transitionError;
                    item.lastError = message;
                    continue;
                }

                const auto& existingEmail = existingResponse.list.front();
                auto currentMailboxIds = existingEmail.mailboxIds;
                std::ranges::sort(currentMailboxIds);
                auto priorMailboxIds = *item.destinationPriorMailboxIds;
                std::ranges::sort(priorMailboxIds);
                if (std::ranges::contains(currentMailboxIds, operation.destinationMailboxId))
                {
                    if (const auto error = requireTransition(
                            repository.markDestinationConfirmed(
                                item.itemId, MailTransferItemPhase::DestinationUnknown,
                                {
                                    .emailId = existingEmail.id,
                                    .blobId = existingEmail.blobId.empty()
                                                  ? std::nullopt
                                                  : std::optional<std::string>{existingEmail.blobId},
                                    .threadId = existingEmail.threadId.empty()
                                                    ? std::nullopt
                                                    : std::optional<std::string>{
                                                          existingEmail.threadId},
                                    .size = existingEmail.size,
                                    .reusedExisting = true,
                                    .priorMailboxIds = priorMailboxIds,
                                }),
                            i18n("The transfer state changed while confirming reconciled "
                                 "destination membership.")))
                        co_return *error;
                    item.destinationBlobId = existingEmail.blobId;
                    item.destinationThreadId = existingEmail.threadId;
                    item.destinationSize = existingEmail.size;
                    item.phase = MailTransferItemPhase::DestinationConfirmed;
                }
                else if (existingResponse.state == *item.destinationPreState &&
                         currentMailboxIds == priorMailboxIds)
                {
                    if (const auto error = requireTransition(
                            repository.transitionItem(item.itemId,
                                                      MailTransferItemPhase::DestinationUnknown,
                                                      MailTransferItemPhase::Uploaded),
                            i18n("The transfer state changed while retrying a proven-absent "
                                 "destination membership update.")))
                        co_return *error;
                    item.phase = MailTransferItemPhase::Uploaded;
                    item.lastError = std::nullopt;
                }
                else
                {
                    const QString message = i18n(
                        "The destination changed while an earlier mailbox update was ambiguous. "
                        "Javelin cannot safely retry it automatically.");
                    if (const auto transitionError = requireTransition(
                            repository.transitionItem(
                                item.itemId, MailTransferItemPhase::DestinationUnknown,
                                MailTransferItemPhase::DestinationUnknown, message),
                            i18n("The transfer state changed while reconciling the destination.")))
                        co_return *transitionError;
                    item.lastError = message;
                    continue;
                }
            }

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
                const auto nextPhase = sameSessionCopy ? MailTransferItemPhase::Uploaded
                                                       : MailTransferItemPhase::AcquiringSource;
                if (const auto error = requireTransition(
                        repository.transitionItem(item.itemId, MailTransferItemPhase::Prepared,
                                                  nextPhase),
                        sameSessionCopy
                            ? i18n("The transfer state changed before the JMAP copy.")
                            : i18n("The source transfer state changed before acquisition.")))
                    co_return *error;
                item.phase = nextPhase;
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

                if (!uploadContext.has_value())
                    co_return stateError(i18n("The transfer has no destination upload context."));
                auto upload = co_await javelin::jmap::api::uploadBlobFromFile(
                    m_resourceTransport, *uploadContext, operation.destinationAccountId,
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

                javelin::jmap::api::RequestBuilder creationBuilder;
                creationBuilder.useCore().useMail();
                std::variant<
                    javelin::jmap::api::CallHandle<javelin::jmap::api::EmailImportResponse>,
                    javelin::jmap::api::CallHandle<javelin::jmap::api::EmailCopyResponse>>
                    creationHandle;
                if (crossServerImport)
                {
                    if (!item.destinationUploadBlobId.has_value())
                        co_return stateError(
                            i18n("The transfer is missing its destination upload blob."));
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
                    creationHandle =
                        creationBuilder.call(*importRequest, "mail-transfer-import");
                }
                else
                {
                    javelin::jmap::api::EmailCopyCreate copy{
                        .id = item.sourceEmailId,
                        .mailboxIds = std::unordered_map<std::string, bool>{
                            {operation.destinationMailboxId, true}},
                        .keywords = keywordMap(item.sourceKeywords),
                        .receivedAt = item.sourceReceivedAt,
                    };
                    const auto copyRequest = javelin::jmap::api::emailCopy({
                        .fromAccountId = sourceAccount.remoteAccountId,
                        .ifFromInState = std::nullopt,
                        .accountId = destinationAccount.remoteAccountId,
                        .ifInState = stateResponse.state,
                        .create = {{item.destinationCreationId, std::move(copy)}},
                        .onSuccessDestroyOriginal = false,
                        .destroyFromIfInState = std::nullopt,
                    });
                    if (!copyRequest.has_value())
                        co_return stateError(i18n("Unable to encode the Email/copy request."));
                    creationHandle = creationBuilder.call(*copyRequest, "mail-transfer-copy");
                }

                bool dispatched = false;
                auto creationCalled = co_await methodCaller.call(
                    destinationRequestContext, creationBuilder, {},
                    [&dispatched] { dispatched = true; });
                if (!std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(creationCalled))
                {
                    const auto error = callerError(creationCalled);
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

                const auto& creationEnvelope =
                    std::get<javelin::jmap::api::ResponseEnvelope>(creationCalled);
                DestinationCreationResponse creationResponse;
                if (crossServerImport)
                {
                    const auto importRead =
                        javelin::jmap::api::ResponseReader{creationEnvelope}.require(
                            std::get<javelin::jmap::api::CallHandle<
                                javelin::jmap::api::EmailImportResponse>>(creationHandle));
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
                    const auto& response =
                        std::get<javelin::jmap::api::EmailImportResponse>(importRead);
                    if (response.accountId != destinationAccount.remoteAccountId)
                    {
                        const QString message =
                            i18n("Email/import returned the wrong account id.");
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
                    creationResponse.created = response.created;
                    creationResponse.notCreated = response.notCreated;
                }
                else
                {
                    const auto copyRead =
                        javelin::jmap::api::ResponseReader{creationEnvelope}.require(
                            std::get<javelin::jmap::api::CallHandle<
                                javelin::jmap::api::EmailCopyResponse>>(creationHandle));
                    if (const auto* readError =
                            std::get_if<javelin::jmap::api::ResponseReaderError>(&copyRead))
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
                    const auto& response =
                        std::get<javelin::jmap::api::EmailCopyResponse>(copyRead);
                    if (response.accountId != destinationAccount.remoteAccountId ||
                        response.fromAccountId != sourceAccount.remoteAccountId)
                    {
                        const QString message = i18n("Email/copy returned the wrong account id.");
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
                    creationResponse.created = response.created;
                    creationResponse.notCreated = response.notCreated;
                }

                const auto created = creationResponse.created.find(item.destinationCreationId);
                if (created != creationResponse.created.end())
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
                    const auto rejected =
                        creationResponse.notCreated.find(item.destinationCreationId);
                    if (rejected != creationResponse.notCreated.end() &&
                        rejected->second.type == "alreadyExists" &&
                        rejected->second.existingId.has_value())
                    {
                        const std::string existingId = *rejected->second.existingId;
                        const auto existingRequest = javelin::jmap::api::emailGet({
                            .accountId = destinationAccount.remoteAccountId,
                            .ids = std::vector<std::string>{existingId},
                            .idsReference = std::nullopt,
                            .properties = std::vector<std::string>{
                                "id", "blobId", "threadId", "mailboxIds", "keywords", "size",
                                "receivedAt"},
                        });
                        if (!existingRequest.has_value())
                            co_return stateError(
                                i18n("Unable to encode the existing destination Email/get request."));
                        javelin::jmap::api::RequestBuilder existingBuilder;
                        existingBuilder.useCore().useMail();
                        const auto existingHandle =
                            existingBuilder.call(*existingRequest, "mail-transfer-existing");
                        auto existingCalled =
                            co_await methodCaller.call(destinationRequestContext, existingBuilder);
                        if (!std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(
                                existingCalled))
                        {
                            const auto error = callerError(existingCalled);
                            if (retryable(error))
                            {
                                if (const auto transitionError = requireTransition(
                                        repository.transitionItem(
                                            item.itemId,
                                            MailTransferItemPhase::CreatingDestination,
                                            MailTransferItemPhase::Uploaded, error.message),
                                        i18n("The transfer state changed while retrying duplicate "
                                             "destination lookup.")))
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
                                    i18n("The transfer state changed while failing duplicate "
                                         "destination lookup.")))
                                co_return *transitionError;
                            if (const auto pinError = releaseSourcePin(repository, item.itemId))
                                co_return *pinError;
                            item.phase = MailTransferItemPhase::Failed;
                            item.lastError = error.message;
                            continue;
                        }

                        const auto existingRead = javelin::jmap::api::ResponseReader{
                            std::get<javelin::jmap::api::ResponseEnvelope>(existingCalled)}
                                                      .require(existingHandle);
                        if (const auto* readError =
                                std::get_if<javelin::jmap::api::ResponseReaderError>(&existingRead))
                        {
                            const auto error = javelin::jmap::operationError(*readError);
                            if (const auto transitionError = requireTransition(
                                    repository.transitionItem(
                                        item.itemId, MailTransferItemPhase::CreatingDestination,
                                        MailTransferItemPhase::Failed, error.message),
                                    i18n("The transfer state changed while failing duplicate "
                                         "destination lookup.")))
                                co_return *transitionError;
                            if (const auto pinError = releaseSourcePin(repository, item.itemId))
                                co_return *pinError;
                            item.phase = MailTransferItemPhase::Failed;
                            item.lastError = error.message;
                            continue;
                        }

                        const auto& existingResponse =
                            std::get<javelin::jmap::api::EmailGetResponse>(existingRead);
                        const bool validExisting =
                            existingResponse.accountId == destinationAccount.remoteAccountId &&
                            !existingResponse.state.empty() && existingResponse.list.size() == 1 &&
                            existingResponse.list.front().id == existingId &&
                            existingResponse.notFound.empty();
                        if (!validExisting)
                        {
                            const QString message = i18n(
                                "The destination reported an existing equivalent message, but it "
                                "could not be verified authoritatively.");
                            if (const auto transitionError = requireTransition(
                                    repository.transitionItem(
                                        item.itemId, MailTransferItemPhase::CreatingDestination,
                                        MailTransferItemPhase::Failed, message),
                                    i18n("The transfer state changed while failing duplicate "
                                         "destination verification.")))
                                co_return *transitionError;
                            if (const auto pinError = releaseSourcePin(repository, item.itemId))
                                co_return *pinError;
                            item.phase = MailTransferItemPhase::Failed;
                            item.lastError = message;
                            continue;
                        }

                        const auto& existingEmail = existingResponse.list.front();
                        auto priorMailboxIds = existingEmail.mailboxIds;
                        std::ranges::sort(priorMailboxIds);
                        if (const auto candidateError = requireTransition(
                                repository.markExistingDestinationCandidate(
                                    item.itemId, MailTransferItemPhase::CreatingDestination,
                                    existingId, existingResponse.state, priorMailboxIds),
                                i18n("The transfer state changed while recording the existing "
                                     "destination message.")))
                            co_return *candidateError;
                        item.destinationEmailId = existingId;
                        item.destinationPreState = existingResponse.state;
                        item.destinationPriorMailboxIds = priorMailboxIds;
                        item.reusedExisting = true;

                        if (!std::ranges::contains(priorMailboxIds,
                                                   operation.destinationMailboxId))
                        {
                            javelin::jmap::api::EmailSetUpdate update;
                            update.patch.emplace(
                                javelin::jmap::api::patchPath(
                                    "mailboxIds", operation.destinationMailboxId),
                                true);
                            const auto setRequest = javelin::jmap::api::emailSet({
                                .accountId = destinationAccount.remoteAccountId,
                                .ifInState = existingResponse.state,
                                .create = {},
                                .update = {{existingId, std::move(update)}},
                                .destroy = {},
                            });
                            if (!setRequest.has_value())
                                co_return stateError(
                                    i18n("Unable to encode the destination Email/set request."));
                            javelin::jmap::api::RequestBuilder setBuilder;
                            setBuilder.useCore().useMail();
                            const auto setHandle =
                                setBuilder.call(*setRequest, "mail-transfer-existing-mailbox");
                            bool setDispatched = false;
                            auto setCalled = co_await methodCaller.call(
                                destinationRequestContext, setBuilder, {},
                                [&setDispatched] { setDispatched = true; });
                            if (!std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(
                                    setCalled))
                            {
                                const auto error = callerError(setCalled);
                                if (setDispatched)
                                {
                                    if (const auto transitionError = requireTransition(
                                            repository.transitionItem(
                                                item.itemId,
                                                MailTransferItemPhase::CreatingDestination,
                                                MailTransferItemPhase::DestinationUnknown,
                                                error.message),
                                            i18n("The transfer state changed while recording an "
                                                 "unknown duplicate membership outcome.")))
                                        co_return *transitionError;
                                    item.phase = MailTransferItemPhase::DestinationUnknown;
                                    item.lastError = error.message;
                                    continue;
                                }
                                if (retryable(error))
                                {
                                    if (const auto transitionError = requireTransition(
                                            repository.transitionItem(
                                                item.itemId,
                                                MailTransferItemPhase::CreatingDestination,
                                                MailTransferItemPhase::Uploaded, error.message),
                                            i18n("The transfer state changed after a pre-dispatch "
                                                 "duplicate membership failure.")))
                                        co_return *transitionError;
                                    item.phase = MailTransferItemPhase::Uploaded;
                                    if (const auto databaseError = repository.updateStatus(
                                            operation.operationId, waitStatus(error), error.message))
                                        co_return javelin::jmap::operationError(*databaseError);
                                    co_return error;
                                }
                                if (const auto transitionError = requireTransition(
                                        repository.transitionItem(
                                            item.itemId,
                                            MailTransferItemPhase::CreatingDestination,
                                            MailTransferItemPhase::Failed, error.message),
                                        i18n("The transfer state changed while failing duplicate "
                                             "membership update.")))
                                    co_return *transitionError;
                                if (const auto pinError =
                                        releaseSourcePin(repository, item.itemId))
                                    co_return *pinError;
                                item.phase = MailTransferItemPhase::Failed;
                                item.lastError = error.message;
                                continue;
                            }

                            const auto setRead = javelin::jmap::api::ResponseReader{
                                std::get<javelin::jmap::api::ResponseEnvelope>(setCalled)}
                                                     .require(setHandle);
                            if (const auto* readError =
                                    std::get_if<javelin::jmap::api::ResponseReaderError>(&setRead))
                            {
                                const auto error = javelin::jmap::operationError(*readError);
                                if (const auto transitionError = requireTransition(
                                        repository.transitionItem(
                                            item.itemId,
                                            MailTransferItemPhase::CreatingDestination,
                                            MailTransferItemPhase::DestinationUnknown,
                                            error.message),
                                        i18n("The transfer state changed while recording an "
                                             "unknown duplicate membership response.")))
                                    co_return *transitionError;
                                item.phase = MailTransferItemPhase::DestinationUnknown;
                                item.lastError = error.message;
                                continue;
                            }

                            const auto& setResponse =
                                std::get<javelin::jmap::api::EmailSetResponse>(setRead);
                            if (setResponse.accountId != destinationAccount.remoteAccountId)
                            {
                                const QString message =
                                    i18n("Email/set returned the wrong destination account id.");
                                if (const auto transitionError = requireTransition(
                                        repository.transitionItem(
                                            item.itemId,
                                            MailTransferItemPhase::CreatingDestination,
                                            MailTransferItemPhase::DestinationUnknown, message),
                                        i18n("The transfer state changed while recording an "
                                             "unknown duplicate membership response.")))
                                    co_return *transitionError;
                                item.phase = MailTransferItemPhase::DestinationUnknown;
                                item.lastError = message;
                                continue;
                            }
                            if (!std::ranges::contains(setResponse.updated, existingId))
                            {
                                if (std::ranges::contains(setResponse.notUpdated, existingId))
                                {
                                    const QString message = i18n(
                                        "The destination rejected adding the existing message to "
                                        "the selected mailbox.");
                                    if (const auto transitionError = requireTransition(
                                            repository.transitionItem(
                                                item.itemId,
                                                MailTransferItemPhase::CreatingDestination,
                                                MailTransferItemPhase::Failed, message),
                                            i18n("The transfer state changed while recording "
                                                 "duplicate membership rejection.")))
                                        co_return *transitionError;
                                    if (const auto pinError =
                                            releaseSourcePin(repository, item.itemId))
                                        co_return *pinError;
                                    item.phase = MailTransferItemPhase::Failed;
                                    item.lastError = message;
                                    continue;
                                }

                                const QString message = i18n(
                                    "Email/set did not account for the existing destination "
                                    "message membership update.");
                                if (const auto transitionError = requireTransition(
                                        repository.transitionItem(
                                            item.itemId,
                                            MailTransferItemPhase::CreatingDestination,
                                            MailTransferItemPhase::DestinationUnknown, message),
                                        i18n("The transfer state changed while recording an "
                                             "unknown duplicate membership response.")))
                                    co_return *transitionError;
                                item.phase = MailTransferItemPhase::DestinationUnknown;
                                item.lastError = message;
                                continue;
                            }
                        }

                        if (item.phase == MailTransferItemPhase::CreatingDestination)
                        {
                            if (const auto confirmedError = requireTransition(
                                    repository.markDestinationConfirmed(
                                        item.itemId,
                                        MailTransferItemPhase::CreatingDestination,
                                        {
                                            .emailId = existingEmail.id,
                                            .blobId = existingEmail.blobId.empty()
                                                          ? std::nullopt
                                                          : std::optional<std::string>{
                                                                existingEmail.blobId},
                                            .threadId = existingEmail.threadId.empty()
                                                            ? std::nullopt
                                                            : std::optional<std::string>{
                                                                  existingEmail.threadId},
                                            .size = existingEmail.size,
                                            .reusedExisting = true,
                                            .priorMailboxIds = priorMailboxIds,
                                        }),
                                    i18n("The transfer state changed while confirming the "
                                         "existing destination message.")))
                                co_return *confirmedError;
                            item.destinationBlobId = existingEmail.blobId;
                            item.destinationThreadId = existingEmail.threadId;
                            item.destinationSize = existingEmail.size;
                            item.phase = MailTransferItemPhase::DestinationConfirmed;
                        }
                    }
                    if (item.phase == MailTransferItemPhase::CreatingDestination &&
                        rejected != creationResponse.notCreated.end())
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

                    if (item.phase == MailTransferItemPhase::CreatingDestination)
                    {
                        const QString message = i18n(
                            "The destination creation response did not account for the requested "
                            "message.");
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
            }

            if (item.phase == MailTransferItemPhase::DestinationConfirmed)
            {
                if (!item.destinationEmailId.has_value())
                    co_return stateError(i18n("The confirmed transfer has no destination Email id."));

                const auto destinationEmailRequest = javelin::jmap::api::emailGet({
                    .accountId = destinationAccount.remoteAccountId,
                    .ids = std::vector<std::string>{*item.destinationEmailId},
                    .idsReference = std::nullopt,
                    .properties = destinationEmailProperties(),
                });
                if (!destinationEmailRequest.has_value())
                    co_return stateError(
                        i18n("Unable to encode the confirmed destination Email/get request."));
                javelin::jmap::api::RequestBuilder destinationEmailBuilder;
                destinationEmailBuilder.useCore().useMail();
                const auto destinationEmailHandle = destinationEmailBuilder.call(
                    *destinationEmailRequest, "mail-transfer-destination-materialize");
                auto destinationEmailCalled =
                    co_await methodCaller.call(destinationRequestContext, destinationEmailBuilder);
                if (!std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(
                        destinationEmailCalled))
                {
                    const auto error = callerError(destinationEmailCalled);
                    if (retryable(error))
                    {
                        if (const auto databaseError = repository.updateStatus(
                                operation.operationId, waitStatus(error), error.message))
                            co_return javelin::jmap::operationError(*databaseError);
                        co_return error;
                    }
                    if (const auto transitionError = requireTransition(
                            repository.transitionItem(
                                item.itemId, MailTransferItemPhase::DestinationConfirmed,
                                MailTransferItemPhase::DestinationUnknown, error.message),
                            i18n("The transfer state changed while validating the confirmed "
                                 "destination.")))
                        co_return *transitionError;
                    item.phase = MailTransferItemPhase::DestinationUnknown;
                    item.lastError = error.message;
                    continue;
                }

                const auto destinationEmailRead = javelin::jmap::api::ResponseReader{
                    std::get<javelin::jmap::api::ResponseEnvelope>(destinationEmailCalled)}
                                                      .require(destinationEmailHandle);
                if (const auto* readError =
                        std::get_if<javelin::jmap::api::ResponseReaderError>(&destinationEmailRead))
                {
                    const auto error = javelin::jmap::operationError(*readError);
                    if (const auto transitionError = requireTransition(
                            repository.transitionItem(
                                item.itemId, MailTransferItemPhase::DestinationConfirmed,
                                MailTransferItemPhase::DestinationUnknown, error.message),
                            i18n("The transfer state changed while validating the confirmed "
                                 "destination.")))
                        co_return *transitionError;
                    item.phase = MailTransferItemPhase::DestinationUnknown;
                    item.lastError = error.message;
                    continue;
                }

                const auto& destinationEmailResponse =
                    std::get<javelin::jmap::api::EmailGetResponse>(destinationEmailRead);
                const bool validDestination =
                    destinationEmailResponse.accountId == destinationAccount.remoteAccountId &&
                    destinationEmailResponse.list.size() == 1 &&
                    destinationEmailResponse.list.front().id == *item.destinationEmailId &&
                    destinationEmailResponse.notFound.empty();
                if (!validDestination ||
                    !std::ranges::contains(destinationEmailResponse.list.front().mailboxIds,
                                           operation.destinationMailboxId))
                {
                    const QString message = i18n(
                        "The confirmed destination message could not be verified in the selected "
                        "mailbox.");
                    if (const auto transitionError = requireTransition(
                            repository.transitionItem(
                                item.itemId, MailTransferItemPhase::DestinationConfirmed,
                                MailTransferItemPhase::DestinationUnknown, message),
                            i18n("The transfer state changed while validating the confirmed "
                                 "destination.")))
                        co_return *transitionError;
                    item.phase = MailTransferItemPhase::DestinationUnknown;
                    item.lastError = message;
                    continue;
                }

                auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
                    m_databaseConnection, QStringLiteral("Materialize mail transfer destination"));
                if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
                    co_return javelin::jmap::operationError(*error);
                auto transaction =
                    std::get<javelin::jmap::cache::DatabaseTransaction>(
                        std::move(transactionResult));
                javelin::jmap::cache::EmailRepository destinationEmails{m_databaseConnection};
                if (const auto error = destinationEmails.upsertMany(
                        transaction, operation.destinationAccountId,
                        destinationEmailResponse.list))
                    co_return javelin::jmap::operationError(*error);
                javelin::jmap::cache::MailboxWindowRepository mailboxWindows{
                    m_databaseConnection};
                if (const auto error = mailboxWindows.invalidateMailbox(
                        transaction, operation.destinationAccountId,
                        operation.destinationMailboxId))
                    co_return javelin::jmap::operationError(*error);
                javelin::jmap::cache::SearchWindowRepository searchWindows{m_databaseConnection};
                if (const auto error =
                        searchWindows.invalidateAccount(transaction, operation.destinationAccountId))
                    co_return javelin::jmap::operationError(*error);
                if (const auto error = transaction.commit())
                    co_return javelin::jmap::operationError(*error);
            }

            if (item.phase == MailTransferItemPhase::DestinationConfirmed &&
                operation.operation == MailTransferOperation::Move)
            {
                if (const auto error = requireTransition(
                        repository.transitionItem(item.itemId,
                                                  MailTransferItemPhase::DestinationConfirmed,
                                                  MailTransferItemPhase::RemovingSource),
                        i18n("The transfer state changed before source cleanup.")))
                    co_return *error;
                item.phase = MailTransferItemPhase::RemovingSource;
            }

            if (item.phase == MailTransferItemPhase::RemovingSource)
            {
                auto cleanupRecordsResult = sourceMutationJournal.listForOperationGroup(
                    operation.sourceAccountId, operation.operationId);
                if (const auto* error = std::get_if<DatabaseError>(&cleanupRecordsResult))
                    co_return javelin::jmap::operationError(*error);
                auto cleanupRecords =
                    std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(
                        std::move(cleanupRecordsResult));
                std::erase_if(cleanupRecords,
                              [&item](const auto& record)
                              { return record.patch.emailId != item.sourceEmailId; });

                const auto hasStatus = [&cleanupRecords](const javelin::jmap::sync::MutationStatus status)
                {
                    return std::ranges::any_of(cleanupRecords, [status](const auto& record)
                                               { return record.status == status; });
                };
                if (hasStatus(javelin::jmap::sync::MutationStatus::Unknown) ||
                    hasStatus(javelin::jmap::sync::MutationStatus::InFlight))
                {
                    const QString message = i18n(
                        "The source cleanup outcome is unknown and must be reconciled before this "
                        "move can continue.");
                    if (const auto error = requireTransition(
                            repository.transitionItem(item.itemId,
                                                      MailTransferItemPhase::RemovingSource,
                                                      MailTransferItemPhase::SourceCleanupUnknown,
                                                      message),
                            i18n("The transfer state changed while recording unknown source "
                                 "cleanup.")))
                        co_return *error;
                    item.phase = MailTransferItemPhase::SourceCleanupUnknown;
                    item.lastError = message;
                    continue;
                }
                if (hasStatus(javelin::jmap::sync::MutationStatus::Rejected))
                {
                    const QString message = i18n(
                        "The destination was created, but the source server rejected cleanup. The "
                        "source message was retained.");
                    if (const auto error = requireTransition(
                            repository.transitionItem(item.itemId,
                                                      MailTransferItemPhase::RemovingSource,
                                                      MailTransferItemPhase::PartialSourceRetained,
                                                      message),
                            i18n("The transfer state changed while recording rejected source "
                                 "cleanup.")))
                        co_return *error;
                    if (const auto pinError = releaseSourcePin(repository, item.itemId))
                        co_return *pinError;
                    item.phase = MailTransferItemPhase::PartialSourceRetained;
                    item.lastError = message;
                    continue;
                }
                if (hasStatus(javelin::jmap::sync::MutationStatus::Accepted))
                {
                    if (const auto error = requireTransition(
                            repository.transitionItem(item.itemId,
                                                      MailTransferItemPhase::RemovingSource,
                                                      MailTransferItemPhase::Complete),
                            i18n("The transfer state changed while completing source cleanup.")))
                        co_return *error;
                    if (!item.sourceDestroy)
                    {
                        if (const auto pinError = releaseSourcePin(repository, item.itemId))
                            co_return *pinError;
                    }
                    item.phase = MailTransferItemPhase::Complete;
                    continue;
                }

                if (cleanupRecords.empty())
                {
                    auto authoritativeResult = co_await sourceMutationEngine.getAuthoritative(
                        liveSettings(*sourceSettings), operation.sourceAccountId,
                        {item.sourceEmailId});
                    if (const auto* error = std::get_if<OperationError>(&authoritativeResult))
                    {
                        if (retryable(*error))
                        {
                            if (const auto databaseError = repository.updateStatus(
                                    operation.operationId, waitStatus(*error), error->message))
                                co_return javelin::jmap::operationError(*databaseError);
                        }
                        co_return *error;
                    }
                    const auto& authoritative =
                        std::get<javelin::jmap::AuthoritativeEmails>(authoritativeResult);
                    const bool missing =
                        authoritative.emails.empty() && authoritative.notFound.size() == 1 &&
                        authoritative.notFound.front() == item.sourceEmailId;
                    if (missing)
                    {
                        if (const auto error = requireTransition(
                                repository.markSourceCleanupPrepared(
                                    item.itemId, MailTransferItemPhase::RemovingSource,
                                    authoritative.state, {}, false),
                                i18n("The transfer state changed while recording completed source "
                                     "cleanup.")))
                            co_return *error;
                        item.sourceEmailState = authoritative.state;
                        item.sourceRemoveMailboxIds.clear();
                        item.sourceDestroy = false;
                        if (const auto error = requireTransition(
                                repository.transitionItem(item.itemId,
                                                          MailTransferItemPhase::RemovingSource,
                                                          MailTransferItemPhase::Complete),
                                i18n("The transfer state changed while completing source cleanup.")))
                            co_return *error;
                        if (const auto pinError = releaseSourcePin(repository, item.itemId))
                            co_return *pinError;
                        item.phase = MailTransferItemPhase::Complete;
                        continue;
                    }
                    if (authoritative.emails.size() != 1 ||
                        authoritative.emails.front().id != item.sourceEmailId ||
                        !authoritative.notFound.empty() || authoritative.state.empty())
                    {
                        co_return OperationError{
                            .code = OperationErrorCode::ProtocolViolation,
                            .message = i18n("The source Email/get response could not be accounted "
                                           "for exactly."),
                        };
                    }

                    const auto& currentEmail = authoritative.emails.front();
                    if (currentEmail.mailboxIds.empty())
                    {
                        co_return OperationError{
                            .code = OperationErrorCode::ProtocolViolation,
                            .message = i18n("The source server returned an Email with no mailbox "
                                           "membership."),
                        };
                    }
                    std::vector<std::string> effectiveRemoveMailboxIds;
                    effectiveRemoveMailboxIds.reserve(item.sourceRemoveMailboxIds.size());
                    for (const auto& mailboxId : item.sourceRemoveMailboxIds)
                    {
                        if (std::ranges::contains(currentEmail.mailboxIds, mailboxId))
                            effectiveRemoveMailboxIds.push_back(mailboxId);
                    }
                    std::ranges::sort(effectiveRemoveMailboxIds);
                    effectiveRemoveMailboxIds.erase(
                        std::unique(effectiveRemoveMailboxIds.begin(),
                                    effectiveRemoveMailboxIds.end()),
                        effectiveRemoveMailboxIds.end());
                    const bool actualDestroy =
                        !effectiveRemoveMailboxIds.empty() &&
                        std::ranges::all_of(currentEmail.mailboxIds, [&](const auto& mailboxId)
                                            {
                                                return std::ranges::contains(
                                                    effectiveRemoveMailboxIds, mailboxId);
                                            });

                    if (const auto error = requireTransition(
                            repository.markSourceCleanupPrepared(
                                item.itemId, MailTransferItemPhase::RemovingSource,
                                authoritative.state, effectiveRemoveMailboxIds, actualDestroy),
                            i18n("The transfer state changed while preparing source cleanup.")))
                        co_return *error;
                    item.sourceEmailState = authoritative.state;
                    item.sourceRemoveMailboxIds = effectiveRemoveMailboxIds;
                    item.sourceDestroy = actualDestroy;

                    if (effectiveRemoveMailboxIds.empty())
                    {
                        if (const auto error = requireTransition(
                                repository.transitionItem(item.itemId,
                                                          MailTransferItemPhase::RemovingSource,
                                                          MailTransferItemPhase::Complete),
                                i18n("The transfer state changed while completing source cleanup.")))
                            co_return *error;
                        if (const auto pinError = releaseSourcePin(repository, item.itemId))
                            co_return *pinError;
                        item.phase = MailTransferItemPhase::Complete;
                        continue;
                    }

                    if (actualDestroy && !item.rawContentHash.has_value())
                    {
                        auto refreshed = co_await m_messageContentClient.refresh(
                            liveSettings(*sourceSettings), operation.sourceAccountId,
                            item.sourceEmailId);
                        if (const auto* error = std::get_if<OperationError>(&refreshed))
                        {
                            if (retryable(*error))
                            {
                                if (const auto databaseError = repository.updateStatus(
                                        operation.operationId, waitStatus(*error), error->message))
                                    co_return javelin::jmap::operationError(*databaseError);
                            }
                            co_return *error;
                        }
                        if (const auto* unavailable =
                                std::get_if<javelin::jmap::MessageContentUnavailable>(&refreshed))
                            co_return sourceUnavailable(unavailable->message);

                        auto referenceResult = sourceRepository.findReference(
                            operation.sourceAccountId, item.sourceEmailId);
                        if (const auto* error = std::get_if<DatabaseError>(&referenceResult))
                            co_return javelin::jmap::operationError(*error);
                        const auto& reference = std::get<
                            std::optional<javelin::jmap::cache::RawMessageSourceReference>>(
                            referenceResult);
                        if (!reference.has_value() || reference->blobId != item.sourceBlobId)
                        {
                            co_return stateError(i18n(
                                "The exact source MIME required for Move undo is unavailable."));
                        }
                        if (const auto error = requireTransition(
                                repository.pinSourceForCleanup(
                                    item.itemId, MailTransferItemPhase::RemovingSource,
                                    reference->object.contentHash),
                                i18n("The transfer state changed while retaining source content "
                                     "for undo.")))
                            co_return *error;
                        item.rawContentHash = reference->object.contentHash;
                    }

                    const auto activeResult =
                        sourceMutationJournal.listForEmail(operation.sourceAccountId,
                                                           item.sourceEmailId);
                    if (const auto* error = std::get_if<DatabaseError>(&activeResult))
                        co_return javelin::jmap::operationError(*error);
                    const auto& activeRecords =
                        std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(activeResult);
                    const bool hasForeignActiveMutation =
                        std::ranges::any_of(activeRecords, [&](const auto& record)
                                            {
                                                const bool active =
                                                    record.status ==
                                                        javelin::jmap::sync::MutationStatus::Pending ||
                                                    record.status ==
                                                        javelin::jmap::sync::MutationStatus::InFlight ||
                                                    record.status ==
                                                        javelin::jmap::sync::MutationStatus::Unknown;
                                                return active &&
                                                       record.operationGroupId !=
                                                           std::optional<std::string>{
                                                               operation.operationId};
                                            });
                    if (hasForeignActiveMutation)
                    {
                        co_return conflictError(i18n(
                            "Another unresolved change is already pending for the source message."));
                    }

                    auto queued = sourceMutationEngine.queue(
                        operation.sourceAccountId,
                        {
                            .emailId = item.sourceEmailId,
                            .addMailboxIds = {},
                            .removeMailboxIds = actualDestroy
                                                    ? std::vector<std::string>{}
                                                    : effectiveRemoveMailboxIds,
                            .addKeywords = {},
                            .removeKeywords = {},
                            .operationGroupId = operation.operationId,
                            .ifInState = authoritative.state,
                            .authoritativeMailboxIds = currentEmail.mailboxIds,
                            .authoritativeKeywords = currentEmail.keywords,
                            .destroy = actualDestroy,
                        });
                    if (const auto* error = std::get_if<OperationError>(&queued))
                        co_return *error;

                    cleanupRecordsResult = sourceMutationJournal.listForOperationGroup(
                        operation.sourceAccountId, operation.operationId);
                    if (const auto* error = std::get_if<DatabaseError>(&cleanupRecordsResult))
                        co_return javelin::jmap::operationError(*error);
                    cleanupRecords =
                        std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(
                            std::move(cleanupRecordsResult));
                    std::erase_if(cleanupRecords,
                                  [&item](const auto& record)
                                  { return record.patch.emailId != item.sourceEmailId; });
                }

                const bool pendingCleanup =
                    std::ranges::any_of(cleanupRecords, [](const auto& record)
                                        {
                                            return record.status ==
                                                   javelin::jmap::sync::MutationStatus::Pending;
                                        });
                if (pendingCleanup)
                {
                    auto submitted = co_await sourceMutationEngine.submitPending(
                        liveSettings(*sourceSettings), operation.sourceAccountId,
                        operation.operationId, 1);
                    if (const auto* submitError = std::get_if<OperationError>(&submitted))
                    {
                        auto afterErrorResult = sourceMutationJournal.listForOperationGroup(
                            operation.sourceAccountId, operation.operationId);
                        if (const auto* error = std::get_if<DatabaseError>(&afterErrorResult))
                            co_return javelin::jmap::operationError(*error);
                        auto afterError =
                            std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(
                                std::move(afterErrorResult));
                        std::erase_if(afterError,
                                      [&item](const auto& record)
                                      { return record.patch.emailId != item.sourceEmailId; });
                        const auto afterHasStatus =
                            [&afterError](const javelin::jmap::sync::MutationStatus status)
                        {
                            return std::ranges::any_of(afterError, [status](const auto& record)
                                                       { return record.status == status; });
                        };
                        if (afterHasStatus(javelin::jmap::sync::MutationStatus::Unknown) ||
                            afterHasStatus(javelin::jmap::sync::MutationStatus::InFlight))
                        {
                            if (const auto error = requireTransition(
                                    repository.transitionItem(
                                        item.itemId, MailTransferItemPhase::RemovingSource,
                                        MailTransferItemPhase::SourceCleanupUnknown,
                                        submitError->message),
                                    i18n("The transfer state changed while recording unknown "
                                         "source cleanup.")))
                                co_return *error;
                            item.phase = MailTransferItemPhase::SourceCleanupUnknown;
                            item.lastError = submitError->message;
                            continue;
                        }
                        if (afterHasStatus(javelin::jmap::sync::MutationStatus::Rejected))
                        {
                            const QString message = i18n(
                                "The destination was created, but source cleanup was rejected. "
                                "The source message was retained.");
                            if (const auto error = requireTransition(
                                    repository.transitionItem(
                                        item.itemId, MailTransferItemPhase::RemovingSource,
                                        MailTransferItemPhase::PartialSourceRetained, message),
                                    i18n("The transfer state changed while recording rejected "
                                         "source cleanup.")))
                                co_return *error;
                            if (const auto pinError = releaseSourcePin(repository, item.itemId))
                                co_return *pinError;
                            item.phase = MailTransferItemPhase::PartialSourceRetained;
                            item.lastError = message;
                            continue;
                        }
                        if (afterHasStatus(javelin::jmap::sync::MutationStatus::Pending) &&
                            retryable(*submitError))
                        {
                            if (const auto databaseError = repository.updateStatus(
                                    operation.operationId, waitStatus(*submitError),
                                    submitError->message))
                                co_return javelin::jmap::operationError(*databaseError);
                        }
                        co_return *submitError;
                    }

                    const auto& submittedSummary =
                        std::get<javelin::jmap::SubmittedEmailMutations>(submitted);
                    const auto submittedItem =
                        std::ranges::find(submittedSummary.items, item.sourceEmailId,
                                          &javelin::jmap::SubmittedEmailMutations::Item::emailId);
                    if (submittedItem == submittedSummary.items.end() || !submittedItem->accepted)
                    {
                        const QString message =
                            submittedItem != submittedSummary.items.end() &&
                                    submittedItem->error.has_value()
                                ? QString::fromStdString(*submittedItem->error)
                                : i18n("The destination was created, but source cleanup was "
                                       "rejected. The source message was retained.");
                        if (const auto error = requireTransition(
                                repository.transitionItem(
                                    item.itemId, MailTransferItemPhase::RemovingSource,
                                    MailTransferItemPhase::PartialSourceRetained, message),
                                i18n("The transfer state changed while recording rejected source "
                                     "cleanup.")))
                            co_return *error;
                        if (const auto pinError = releaseSourcePin(repository, item.itemId))
                            co_return *pinError;
                        item.phase = MailTransferItemPhase::PartialSourceRetained;
                        item.lastError = message;
                        continue;
                    }

                    if (const auto error = requireTransition(
                            repository.transitionItem(item.itemId,
                                                      MailTransferItemPhase::RemovingSource,
                                                      MailTransferItemPhase::Complete),
                            i18n("The transfer state changed while completing source cleanup.")))
                        co_return *error;
                    if (!item.sourceDestroy)
                    {
                        if (const auto pinError = releaseSourcePin(repository, item.itemId))
                            co_return *pinError;
                    }
                    item.phase = MailTransferItemPhase::Complete;
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
            if (item.phase == MailTransferItemPhase::PartialSourceRetained)
                ++summary.partialItemCount;
            if (item.phase == MailTransferItemPhase::DestinationUnknown ||
                item.phase == MailTransferItemPhase::SourceCleanupUnknown)
                ++summary.unknownItemCount;
        }

        if (summary.unknownItemCount > 0)
            summary.status = MailTransferStatus::BlockedUnknown;
        else if (summary.failedItemCount == items.size())
            summary.status = MailTransferStatus::Failed;
        else if (summary.failedItemCount > 0 || summary.partialItemCount > 0)
            summary.status = MailTransferStatus::Partial;
        else if (summary.completeItemCount == items.size())
            summary.status = MailTransferStatus::Complete;
        else
            summary.status = MailTransferStatus::Running;

        if (const auto error = repository.updateStatus(operation.operationId, summary.status))
            co_return javelin::jmap::operationError(*error);
        co_return summary;
    }

} // namespace javelin::app
