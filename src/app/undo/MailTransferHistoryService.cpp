#include "app/undo/MailTransferHistoryService.h"

#include "app/AccountConnectionProvider.h"
#include "app/AccountConnectionSettings.h"
#include "app/MailTransferApplicationService.h"
#include "app/MailTransferExecutor.h"
#include "app/MailTransferRepository.h"
#include "app/MessageSelection.h"
#include "jmap/MessageContentClient.h"
#include "jmap/api/BlobUpload.h"
#include "jmap/api/MailMethods.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/RequestBuilder.h"
#include "jmap/api/ResponseReader.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailVault.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/RawMessageSourceRepository.h"
#include "jmap/cache/SearchWindowRepository.h"
#include "jmap/cache/SessionRepository.h"

#include <KLocalizedString>

#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <algorithm>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace javelin::app::undo
{
    namespace
    {
        using DatabaseError = javelin::jmap::cache::DatabaseError;
        using OperationError = javelin::jmap::OperationError;
        using OperationErrorCode = javelin::jmap::OperationErrorCode;

        struct RecreationRecord
        {
            QString historyEntryId;
            std::string contentHash;
            std::string accountId;
            std::string creationId;
            std::optional<std::string> uploadedBlobId;
            std::optional<std::string> preState;
            std::optional<std::string> emailId;
            QString phase;
        };

        struct ReadAccountContext
        {
            javelin::jmap::cache::CachedAccount account;
            javelin::jmap::api::Session session;
            javelin::app::AccountConnectionSettings settings;
            javelin::jmap::api::ApiRequestContext requestContext;
        };

        struct AccountContext : ReadAccountContext
        {
            javelin::jmap::api::BlobUploadContext uploadContext;
        };

        [[nodiscard]] OperationError dbError(const QString& operation, const QSqlQuery& query)
        {
            return javelin::jmap::operationError(javelin::jmap::cache::databaseError(
                operation, query.lastError(),
                javelin::jmap::cache::DatabaseErrorCode::QueryFailed));
        }

        [[nodiscard]] OperationError invalid(QString message)
        {
            return {.code = OperationErrorCode::PreconditionFailed, .message = std::move(message)};
        }

        [[nodiscard]] OperationError ambiguous(QString message)
        {
            return {
                .code = OperationErrorCode::Conflict,
                .message = std::move(message),
                .httpStatus = std::nullopt,
                .retryAfter = std::nullopt,
                .protocolType = std::optional<std::string>{"ambiguousOutcome"},
            };
        }

        [[nodiscard]] OperationError
        callerError(const javelin::jmap::api::MethodCallerResult& result)
        {
            if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&result))
                return javelin::jmap::operationError(*error);
            if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&result))
                return javelin::jmap::operationError(*error);
            if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&result))
                return javelin::jmap::operationError(*error);
            return {.code = OperationErrorCode::ProtocolViolation,
                    .message = i18n("The JMAP history request returned an invalid result.")};
        }

        [[nodiscard]] javelin::jmap::LiveConnectionSettings
        liveSettings(const javelin::app::AccountConnectionSettings& settings)
        {
            return {
                .sessionUrl = settings.sessionUrl,
                .loginEmail = settings.loginEmail,
                .apiKey = settings.apiKey,
            };
        }

        [[nodiscard]] javelin::jmap::api::ApiRequestContext
        requestContext(const javelin::app::AccountConnectionSettings& settings,
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

        [[nodiscard]] std::unordered_map<std::string, bool>
        enabledMap(const std::vector<std::string>& values)
        {
            std::unordered_map<std::string, bool> result;
            result.reserve(values.size());
            for (const auto& value : values)
                result.emplace(value, true);
            return result;
        }

        [[nodiscard]] bool containsAll(const std::vector<std::string>& values,
                                       const std::vector<std::string>& required)
        {
            return std::ranges::all_of(required, [&](const auto& value)
                                       { return std::ranges::contains(values, value); });
        }

        [[nodiscard]] std::vector<std::string> normalized(std::vector<std::string> values)
        {
            std::ranges::sort(values);
            values.erase(std::unique(values.begin(), values.end()), values.end());
            return values;
        }

        [[nodiscard]] const std::vector<std::string>& emailProperties()
        {
            static const std::vector<std::string> properties{
                "id",         "blobId",        "threadId", "mailboxIds", "keywords",
                "size",       "receivedAt",    "sentAt",   "messageId",  "inReplyTo",
                "references", "hasAttachment", "subject",  "from",       "to",
                "cc",         "bcc",           "replyTo",  "preview",
            };
            return properties;
        }

        [[nodiscard]] std::variant<std::optional<RecreationRecord>, OperationError>
        findRecreation(javelin::jmap::cache::DatabaseConnection& database, const QString& entryId,
                       const std::string& contentHash)
        {
            QSqlQuery query{database.database()};
            query.prepare(QStringLiteral(
                "SELECT history_entry_id,content_hash,account_id,creation_id,uploaded_blob_id,"
                "pre_state,email_id,phase FROM mail_transfer_history_recreations WHERE "
                "history_entry_id=:history_entry_id AND content_hash=:content_hash"));
            query.bindValue(QStringLiteral(":history_entry_id"), entryId);
            query.bindValue(QStringLiteral(":content_hash"), QString::fromStdString(contentHash));
            if (!query.exec())
                return dbError(QStringLiteral("Find mail transfer history recreation"), query);
            if (!query.next())
                return std::optional<RecreationRecord>{std::nullopt};
            return std::optional<RecreationRecord>{RecreationRecord{
                .historyEntryId = query.value(0).toString(),
                .contentHash = query.value(1).toString().toStdString(),
                .accountId = query.value(2).toString().toStdString(),
                .creationId = query.value(3).toString().toStdString(),
                .uploadedBlobId =
                    query.value(4).isNull()
                        ? std::nullopt
                        : std::optional<std::string>{query.value(4).toString().toStdString()},
                .preState =
                    query.value(5).isNull()
                        ? std::nullopt
                        : std::optional<std::string>{query.value(5).toString().toStdString()},
                .emailId =
                    query.value(6).isNull()
                        ? std::nullopt
                        : std::optional<std::string>{query.value(6).toString().toStdString()},
                .phase = query.value(7).toString(),
            }};
        }

        [[nodiscard]] std::optional<OperationError>
        createRecreation(javelin::jmap::cache::DatabaseConnection& database,
                         const RecreationRecord& record)
        {
            QSqlQuery query{database.database()};
            query.prepare(QStringLiteral(
                "INSERT OR IGNORE INTO mail_transfer_history_recreations("
                "history_entry_id,content_hash,account_id,creation_id,phase) VALUES("
                ":history_entry_id,:content_hash,:account_id,:creation_id,'prepared')"));
            query.bindValue(QStringLiteral(":history_entry_id"), record.historyEntryId);
            query.bindValue(QStringLiteral(":content_hash"),
                            QString::fromStdString(record.contentHash));
            query.bindValue(QStringLiteral(":account_id"),
                            QString::fromStdString(record.accountId));
            query.bindValue(QStringLiteral(":creation_id"),
                            QString::fromStdString(record.creationId));
            const javelin::jmap::cache::DatabaseWriteScope writeScope{database};
            if (!query.exec())
                return dbError(QStringLiteral("Create mail transfer history recreation"), query);
            return std::nullopt;
        }

        [[nodiscard]] std::optional<OperationError>
        setUploaded(javelin::jmap::cache::DatabaseConnection& database,
                    const RecreationRecord& record, const std::string& blobId)
        {
            QSqlQuery query{database.database()};
            query.prepare(QStringLiteral(
                "UPDATE mail_transfer_history_recreations SET uploaded_blob_id=:blob_id,"
                "phase='uploaded',last_error=NULL,updated_at=CURRENT_TIMESTAMP WHERE "
                "history_entry_id=:history_entry_id AND content_hash=:content_hash AND "
                "phase='prepared'"));
            query.bindValue(QStringLiteral(":blob_id"), QString::fromStdString(blobId));
            query.bindValue(QStringLiteral(":history_entry_id"), record.historyEntryId);
            query.bindValue(QStringLiteral(":content_hash"),
                            QString::fromStdString(record.contentHash));
            const javelin::jmap::cache::DatabaseWriteScope writeScope{database};
            if (!query.exec())
                return dbError(QStringLiteral("Record history source upload"), query);
            if (query.numRowsAffected() != 1)
                return invalid(i18n("The source recreation state changed while recording upload."));
            return std::nullopt;
        }

        [[nodiscard]] std::optional<OperationError>
        setCreating(javelin::jmap::cache::DatabaseConnection& database,
                    const RecreationRecord& record, const std::string& preState)
        {
            QSqlQuery query{database.database()};
            query.prepare(QStringLiteral(
                "UPDATE mail_transfer_history_recreations SET pre_state=:pre_state,"
                "phase='creating',last_error=NULL,updated_at=CURRENT_TIMESTAMP WHERE "
                "history_entry_id=:history_entry_id AND content_hash=:content_hash AND "
                "phase='uploaded'"));
            query.bindValue(QStringLiteral(":pre_state"), QString::fromStdString(preState));
            query.bindValue(QStringLiteral(":history_entry_id"), record.historyEntryId);
            query.bindValue(QStringLiteral(":content_hash"),
                            QString::fromStdString(record.contentHash));
            const javelin::jmap::cache::DatabaseWriteScope writeScope{database};
            if (!query.exec())
                return dbError(QStringLiteral("Prepare history source import"), query);
            if (query.numRowsAffected() != 1)
                return invalid(i18n("The source recreation state changed before import."));
            return std::nullopt;
        }

        [[nodiscard]] std::optional<OperationError>
        setPhase(javelin::jmap::cache::DatabaseConnection& database, const RecreationRecord& record,
                 const QString& expected, const QString& next,
                 std::optional<QString> error = std::nullopt)
        {
            QSqlQuery query{database.database()};
            query.prepare(QStringLiteral(
                "UPDATE mail_transfer_history_recreations SET phase=:next,last_error=:last_error,"
                "updated_at=CURRENT_TIMESTAMP WHERE history_entry_id=:history_entry_id AND "
                "content_hash=:content_hash AND phase=:expected"));
            query.bindValue(QStringLiteral(":next"), next);
            query.bindValue(QStringLiteral(":last_error"),
                            error.has_value() ? QVariant{*error} : QVariant{});
            query.bindValue(QStringLiteral(":history_entry_id"), record.historyEntryId);
            query.bindValue(QStringLiteral(":content_hash"),
                            QString::fromStdString(record.contentHash));
            query.bindValue(QStringLiteral(":expected"), expected);
            const javelin::jmap::cache::DatabaseWriteScope writeScope{database};
            if (!query.exec())
                return dbError(QStringLiteral("Transition history source recreation"), query);
            if (query.numRowsAffected() != 1)
                return invalid(i18n("The source recreation state changed unexpectedly."));
            return std::nullopt;
        }

        [[nodiscard]] std::optional<OperationError>
        setComplete(javelin::jmap::cache::DatabaseConnection& database,
                    const RecreationRecord& record, const QString& expected,
                    const std::string& emailId)
        {
            QSqlQuery query{database.database()};
            query.prepare(QStringLiteral(
                "UPDATE mail_transfer_history_recreations SET email_id=:email_id,phase='complete',"
                "last_error=NULL,updated_at=CURRENT_TIMESTAMP WHERE "
                "history_entry_id=:history_entry_id AND content_hash=:content_hash AND "
                "phase=:expected"));
            query.bindValue(QStringLiteral(":email_id"), QString::fromStdString(emailId));
            query.bindValue(QStringLiteral(":history_entry_id"), record.historyEntryId);
            query.bindValue(QStringLiteral(":content_hash"),
                            QString::fromStdString(record.contentHash));
            query.bindValue(QStringLiteral(":expected"), expected);
            const javelin::jmap::cache::DatabaseWriteScope writeScope{database};
            if (!query.exec())
                return dbError(QStringLiteral("Complete history source recreation"), query);
            if (query.numRowsAffected() != 1)
                return invalid(
                    i18n("The source recreation state changed while completing import."));
            return std::nullopt;
        }

        [[nodiscard]] std::variant<ReadAccountContext, OperationError>
        readAccountContext(javelin::jmap::cache::DatabaseConnection& database,
                           const javelin::app::AccountConnectionProvider& connections,
                           const std::string& accountId)
        {
            javelin::jmap::cache::AccountRepository accounts{database};
            const auto accountResult = accounts.findById(accountId);
            if (const auto* error = std::get_if<DatabaseError>(&accountResult))
                return javelin::jmap::operationError(*error);
            const auto& account =
                std::get<std::optional<javelin::jmap::cache::CachedAccount>>(accountResult);
            if (!account.has_value() || account->remoteAccountId.empty())
                return invalid(i18n("The source mail account is no longer available."));

            const auto settings = connections.connectionSettingsFor(accountId);
            if (!settings.has_value())
                return invalid(i18n("Connection settings are unavailable for the source account."));

            javelin::jmap::cache::SessionRepository sessions{database};
            const auto sessionResult = sessions.load(accountId);
            if (const auto* error = std::get_if<DatabaseError>(&sessionResult))
                return javelin::jmap::operationError(*error);
            const auto& session =
                std::get<std::optional<javelin::jmap::api::Session>>(sessionResult);
            if (!session.has_value() || !session->accounts.contains(account->remoteAccountId))
                return invalid(i18n("The source account is absent from its cached JMAP session."));

            auto apiContext = requestContext(*settings, accountId, *session);
            if (!apiContext.requestLimits.has_value())
                return invalid(i18n("The mail server does not advertise usable JMAP limits."));
            return ReadAccountContext{
                .account = *account,
                .session = *session,
                .settings = *settings,
                .requestContext = std::move(apiContext),
            };
        }

        [[nodiscard]] std::variant<AccountContext, OperationError>
        accountContext(javelin::jmap::cache::DatabaseConnection& database,
                       const javelin::app::AccountConnectionProvider& connections,
                       const std::string& accountId)
        {
            auto baseResult = readAccountContext(database, connections, accountId);
            if (const auto* error = std::get_if<OperationError>(&baseResult))
                return *error;
            auto base = std::get<ReadAccountContext>(std::move(baseResult));
            const auto upload =
                javelin::jmap::api::blobUploadContext(base.session, base.account.remoteAccountId);
            if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&upload))
                return javelin::jmap::operationError(*error);
            return AccountContext{
                std::move(base),
                std::get<javelin::jmap::api::BlobUploadContext>(upload),
            };
        }

        [[nodiscard]] std::optional<OperationError>
        verifyHistoryPin(javelin::jmap::cache::DatabaseConnection& database,
                         const QString& historyEntryId, const std::string& contentHash)
        {
            QSqlQuery query{database.database()};
            query.prepare(QStringLiteral(
                "SELECT EXISTS(SELECT 1 FROM mail_vault_pins WHERE owner_kind='history_entry' AND "
                "owner_id=:owner_id AND content_hash=:content_hash)"));
            query.bindValue(QStringLiteral(":owner_id"), historyEntryId);
            query.bindValue(QStringLiteral(":content_hash"), QString::fromStdString(contentHash));
            if (!query.exec() || !query.next())
                return dbError(QStringLiteral("Verify history mail vault pin"), query);
            if (!query.value(0).toBool())
                return invalid(
                    i18n("The raw message retained for this Undo entry is unavailable."));
            return std::nullopt;
        }

        [[nodiscard]] std::variant<std::optional<std::string>, OperationError>
        findRedoOperation(javelin::jmap::cache::DatabaseConnection& database,
                          const QString& historyEntryId, const std::uint64_t redoGeneration,
                          const std::string& sourceEmailId, const std::string& destinationAccountId,
                          const std::string& destinationMailboxId,
                          const MailTransferHistoryOperation operation)
        {
            QSqlQuery query{database.database()};
            query.prepare(QStringLiteral(
                "SELECT transfer_operation_id FROM mail_transfer_history_redos WHERE "
                "history_entry_id=:history_entry_id AND redo_generation=:redo_generation AND "
                "source_email_id=:source_email_id AND "
                "destination_account_id=:destination_account_id "
                "AND destination_mailbox_id=:destination_mailbox_id AND operation=:operation"));
            query.bindValue(QStringLiteral(":history_entry_id"), historyEntryId);
            query.bindValue(QStringLiteral(":redo_generation"),
                            static_cast<qulonglong>(redoGeneration));
            query.bindValue(QStringLiteral(":source_email_id"),
                            QString::fromStdString(sourceEmailId));
            query.bindValue(QStringLiteral(":destination_account_id"),
                            QString::fromStdString(destinationAccountId));
            query.bindValue(QStringLiteral(":destination_mailbox_id"),
                            QString::fromStdString(destinationMailboxId));
            query.bindValue(QStringLiteral(":operation"),
                            operation == MailTransferHistoryOperation::Move
                                ? QStringLiteral("move")
                                : QStringLiteral("copy"));
            if (!query.exec())
                return dbError(QStringLiteral("Find mail transfer history redo"), query);
            if (!query.next())
                return std::optional<std::string>{std::nullopt};
            return std::optional<std::string>{query.value(0).toString().toStdString()};
        }

        [[nodiscard]] std::optional<OperationError> recordRedoOperation(
            javelin::jmap::cache::DatabaseConnection& database, const QString& historyEntryId,
            const std::uint64_t redoGeneration, const std::string& sourceEmailId,
            const std::string& destinationAccountId, const std::string& destinationMailboxId,
            const MailTransferHistoryOperation operation, const std::string& transferOperationId)
        {
            QSqlQuery query{database.database()};
            query.prepare(QStringLiteral(
                "INSERT OR IGNORE INTO mail_transfer_history_redos("
                "history_entry_id,redo_generation,source_email_id,destination_account_id,"
                "destination_mailbox_id,operation,transfer_operation_id) VALUES("
                ":history_entry_id,:redo_generation,:source_email_id,:destination_account_id,"
                ":destination_mailbox_id,:operation,:transfer_operation_id)"));
            query.bindValue(QStringLiteral(":history_entry_id"), historyEntryId);
            query.bindValue(QStringLiteral(":redo_generation"),
                            static_cast<qulonglong>(redoGeneration));
            query.bindValue(QStringLiteral(":source_email_id"),
                            QString::fromStdString(sourceEmailId));
            query.bindValue(QStringLiteral(":destination_account_id"),
                            QString::fromStdString(destinationAccountId));
            query.bindValue(QStringLiteral(":destination_mailbox_id"),
                            QString::fromStdString(destinationMailboxId));
            query.bindValue(QStringLiteral(":operation"),
                            operation == MailTransferHistoryOperation::Move
                                ? QStringLiteral("move")
                                : QStringLiteral("copy"));
            query.bindValue(QStringLiteral(":transfer_operation_id"),
                            QString::fromStdString(transferOperationId));
            const javelin::jmap::cache::DatabaseWriteScope writeScope{database};
            if (!query.exec())
                return dbError(QStringLiteral("Record mail transfer history redo"), query);
            return std::nullopt;
        }

        [[nodiscard]] OperationError rejectedImport(const javelin::jmap::api::SetError& error)
        {
            return {
                .code = error.type == "forbidden" ? OperationErrorCode::PermissionDenied
                                                  : OperationErrorCode::ServerFailure,
                .message = error.description.has_value()
                               ? QString::fromStdString(*error.description)
                               : i18n("The source server rejected restoring this message (%1).",
                                      QString::fromStdString(error.type)),
                .protocolType = error.type,
            };
        }
    } // namespace

    MailTransferHistoryService::MailTransferHistoryService(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::api::AbstractTransport& resourceTransport,
        javelin::jmap::api::JmapMethodTransport& methodTransport,
        const javelin::app::AccountConnectionProvider& connectionProvider)
        : m_databaseConnection(databaseConnection), m_resourceTransport(resourceTransport),
          m_methodTransport(methodTransport), m_connectionProvider(connectionProvider)
    {
    }

    QCoro::Task<javelin::jmap::AuthoritativeEmailsResult>
    MailTransferHistoryService::getAuthoritativeEmails(std::string accountId,
                                                       std::vector<std::string> emailIds)
    {
        auto contextResult =
            readAccountContext(m_databaseConnection, m_connectionProvider, accountId);
        if (const auto* error = std::get_if<OperationError>(&contextResult))
            co_return *error;
        const auto context = std::get<ReadAccountContext>(std::move(contextResult));

        const auto request = javelin::jmap::api::emailGet({
            .accountId = context.account.remoteAccountId,
            .ids = std::move(emailIds),
            .idsReference = std::nullopt,
            .properties = emailProperties(),
        });
        if (!request.has_value())
            co_return invalid(i18n("Unable to encode authoritative Email/get request."));
        javelin::jmap::api::RequestBuilder builder;
        builder.useCore().useMail();
        const auto handle = builder.call(*request, "mail-transfer-history-email-get");
        javelin::jmap::api::MethodCaller caller{m_methodTransport};
        auto called = co_await caller.call(context.requestContext, builder);
        if (!std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(called))
            co_return callerError(called);
        const auto read =
            javelin::jmap::api::ResponseReader{
                std::get<javelin::jmap::api::ResponseEnvelope>(called)}
                .require(handle);
        if (const auto* error = std::get_if<javelin::jmap::api::ResponseReaderError>(&read))
            co_return javelin::jmap::operationError(*error);
        const auto& response = std::get<javelin::jmap::api::EmailGetResponse>(read);
        if (response.accountId != context.account.remoteAccountId || response.state.empty())
            co_return invalid(i18n("The server returned invalid authoritative Email state."));
        co_return javelin::jmap::AuthoritativeEmails{
            .accountId = std::move(accountId),
            .state = response.state,
            .emails = response.list,
            .notFound = response.notFound,
        };
    }

    QCoro::Task<javelin::jmap::SubmittedEmailMutationsResult>
    MailTransferHistoryService::applyExactEmailMutation(
        std::string accountId, javelin::jmap::EmailMailboxMutation mutation)
    {
        const auto settings = m_connectionProvider.connectionSettingsFor(accountId);
        if (!settings.has_value())
            co_return invalid(i18n("Connection settings are unavailable for this mail account."));
        if (!mutation.operationGroupId.has_value())
        {
            mutation.operationGroupId =
                QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        }
        const auto operationGroupId = mutation.operationGroupId;
        javelin::jmap::EmailMutationEngine engine{m_databaseConnection, m_methodTransport};
        const auto queued = engine.queue(accountId, std::move(mutation));
        if (const auto* error = std::get_if<OperationError>(&queued))
            co_return *error;
        co_return co_await engine.submitPending(liveSettings(*settings), std::move(accountId),
                                                operationGroupId, 1);
    }

    QCoro::Task<RecreatedMailTransferSourceResult>
    MailTransferHistoryService::recreateSourceFromHistory(
        QString historyEntryId, std::string accountId, std::string rawContentHash,
        std::vector<std::string> mailboxIds, std::vector<std::string> keywords,
        std::vector<std::string> messageIds, std::optional<std::string> receivedAt,
        const std::uint64_t sourceSize)
    {
        if (historyEntryId.isEmpty() || accountId.empty() || rawContentHash.empty() ||
            mailboxIds.empty())
            co_return invalid(i18n("The source recreation request is incomplete."));
        if (const auto error =
                verifyHistoryPin(m_databaseConnection, historyEntryId, rawContentHash))
            co_return *error;

        auto contextResult = accountContext(m_databaseConnection, m_connectionProvider, accountId);
        if (const auto* error = std::get_if<OperationError>(&contextResult))
            co_return *error;
        const auto context = std::get<AccountContext>(std::move(contextResult));

        auto recordResult = findRecreation(m_databaseConnection, historyEntryId, rawContentHash);
        if (const auto* error = std::get_if<OperationError>(&recordResult))
            co_return *error;
        auto record = std::get<std::optional<RecreationRecord>>(std::move(recordResult));
        if (!record.has_value())
        {
            RecreationRecord created{
                .historyEntryId = historyEntryId,
                .contentHash = rawContentHash,
                .accountId = accountId,
                .creationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
                .uploadedBlobId = std::nullopt,
                .preState = std::nullopt,
                .emailId = std::nullopt,
                .phase = QStringLiteral("prepared"),
            };
            if (const auto error = createRecreation(m_databaseConnection, created))
                co_return *error;
            recordResult = findRecreation(m_databaseConnection, historyEntryId, rawContentHash);
            if (const auto* error = std::get_if<OperationError>(&recordResult))
                co_return *error;
            record = std::get<std::optional<RecreationRecord>>(std::move(recordResult));
            if (!record.has_value())
                co_return invalid(i18n("The source recreation journal could not be created."));
        }
        if (record->accountId != accountId)
            co_return invalid(i18n("The source recreation journal belongs to another account."));

        javelin::jmap::api::MethodCaller caller{m_methodTransport};
        for (int step = 0; step < 8; ++step)
        {
            if (record->phase == QStringLiteral("complete"))
            {
                if (!record->emailId.has_value())
                    co_return invalid(
                        i18n("The completed source recreation is missing its Email id."));
                const auto emailRequest = javelin::jmap::api::emailGet({
                    .accountId = context.account.remoteAccountId,
                    .ids = std::vector<std::string>{*record->emailId},
                    .idsReference = std::nullopt,
                    .properties = emailProperties(),
                });
                if (!emailRequest.has_value())
                    co_return invalid(i18n("Unable to encode recreated source Email/get request."));
                javelin::jmap::api::RequestBuilder emailBuilder;
                emailBuilder.useCore().useMail();
                const auto emailHandle =
                    emailBuilder.call(*emailRequest, "history-source-materialize");
                auto emailCalled = co_await caller.call(context.requestContext, emailBuilder);
                if (!std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(emailCalled))
                    co_return callerError(emailCalled);
                const auto emailRead =
                    javelin::jmap::api::ResponseReader{
                        std::get<javelin::jmap::api::ResponseEnvelope>(emailCalled)}
                        .require(emailHandle);
                if (const auto* error =
                        std::get_if<javelin::jmap::api::ResponseReaderError>(&emailRead))
                    co_return javelin::jmap::operationError(*error);
                const auto& response = std::get<javelin::jmap::api::EmailGetResponse>(emailRead);
                if (response.accountId != context.account.remoteAccountId ||
                    response.list.size() != 1 || response.list.front().id != *record->emailId ||
                    !response.notFound.empty())
                    co_return invalid(
                        i18n("The recreated source Email is not available after import."));

                auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
                    m_databaseConnection, QStringLiteral("Materialize recreated source Email"));
                if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
                    co_return javelin::jmap::operationError(*error);
                auto transaction = std::get<javelin::jmap::cache::DatabaseTransaction>(
                    std::move(transactionResult));
                javelin::jmap::cache::EmailRepository emails{m_databaseConnection};
                if (const auto error = emails.upsertMany(transaction, accountId, response.list))
                    co_return javelin::jmap::operationError(*error);
                javelin::jmap::cache::MailboxWindowRepository mailboxWindows{m_databaseConnection};
                for (const auto& mailboxId : response.list.front().mailboxIds)
                    if (const auto error =
                            mailboxWindows.invalidateMailbox(transaction, accountId, mailboxId))
                        co_return javelin::jmap::operationError(*error);
                javelin::jmap::cache::SearchWindowRepository searchWindows{m_databaseConnection};
                if (const auto error = searchWindows.invalidateAccount(transaction, accountId))
                    co_return javelin::jmap::operationError(*error);
                if (const auto error = transaction.commit())
                    co_return javelin::jmap::operationError(*error);

                co_return RecreatedMailTransferSource{
                    .emailId = response.list.front().id,
                    .blobId = response.list.front().blobId.empty()
                                  ? std::nullopt
                                  : std::optional<std::string>{response.list.front().blobId},
                    .threadId = response.list.front().threadId.empty()
                                    ? std::nullopt
                                    : std::optional<std::string>{response.list.front().threadId},
                    .size = response.list.front().size,
                };
            }

            if (record->phase == QStringLiteral("creating"))
            {
                if (const auto error =
                        setPhase(m_databaseConnection, *record, QStringLiteral("creating"),
                                 QStringLiteral("unknown"),
                                 i18n("The application stopped while source "
                                      "recreation may have been dispatched.")))
                    co_return *error;
                record->phase = QStringLiteral("unknown");
            }

            if (record->phase == QStringLiteral("prepared"))
            {
                javelin::jmap::cache::RawMessageSourceRepository sources{m_databaseConnection};
                const auto objectResult = sources.findVaultObject(rawContentHash);
                if (const auto* error = std::get_if<DatabaseError>(&objectResult))
                    co_return javelin::jmap::operationError(*error);
                const auto& object =
                    std::get<std::optional<javelin::jmap::cache::MailVaultObject>>(objectResult);
                if (!object.has_value())
                    co_return invalid(
                        i18n("The retained raw source is missing from the mail vault."));
                const auto vault =
                    javelin::jmap::cache::MailVault::forDatabase(m_databaseConnection);
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
                const auto upload = co_await javelin::jmap::api::uploadBlobFromFile(
                    m_resourceTransport, context.uploadContext, accountId,
                    context.account.remoteAccountId, context.settings.apiKey,
                    std::get<QString>(pathResult), "message/rfc822");
                if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&upload))
                    co_return javelin::jmap::operationError(*error);
                if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&upload))
                    co_return javelin::jmap::operationError(*error);
                const auto& uploaded = std::get<javelin::jmap::api::BlobUploadResponse>(upload);
                if (uploaded.accountId != context.account.remoteAccountId)
                    co_return invalid(i18n("The upload response returned the wrong account id."));
                if (const auto error = setUploaded(m_databaseConnection, *record, uploaded.blobId))
                    co_return *error;
                record->uploadedBlobId = uploaded.blobId;
                record->phase = QStringLiteral("uploaded");
                continue;
            }

            if (record->phase == QStringLiteral("uploaded"))
            {
                if (!record->uploadedBlobId.has_value())
                    co_return invalid(i18n("The source recreation is missing its uploaded blob."));
                const auto stateRequest = javelin::jmap::api::emailGet({
                    .accountId = context.account.remoteAccountId,
                    .ids = std::vector<std::string>{},
                    .idsReference = std::nullopt,
                    .properties = std::vector<std::string>{"id"},
                });
                if (!stateRequest.has_value())
                    co_return invalid(i18n("Unable to encode source Email state request."));
                javelin::jmap::api::RequestBuilder stateBuilder;
                stateBuilder.useCore().useMail();
                const auto stateHandle = stateBuilder.call(*stateRequest, "history-source-state");
                auto stateCalled = co_await caller.call(context.requestContext, stateBuilder);
                if (!std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(stateCalled))
                    co_return callerError(stateCalled);
                const auto stateRead =
                    javelin::jmap::api::ResponseReader{
                        std::get<javelin::jmap::api::ResponseEnvelope>(stateCalled)}
                        .require(stateHandle);
                if (const auto* error =
                        std::get_if<javelin::jmap::api::ResponseReaderError>(&stateRead))
                    co_return javelin::jmap::operationError(*error);
                const auto& state = std::get<javelin::jmap::api::EmailGetResponse>(stateRead);
                if (state.accountId != context.account.remoteAccountId || state.state.empty())
                    co_return invalid(i18n("The source Email state response is invalid."));
                if (const auto error = setCreating(m_databaseConnection, *record, state.state))
                    co_return *error;
                record->preState = state.state;
                record->phase = QStringLiteral("creating");

                const auto importRequest = javelin::jmap::api::emailImport({
                    .accountId = context.account.remoteAccountId,
                    .ifInState = state.state,
                    .emails = {{record->creationId,
                                javelin::jmap::api::EmailImport{
                                    .blobId = *record->uploadedBlobId,
                                    .mailboxIds = enabledMap(mailboxIds),
                                    .keywords = enabledMap(keywords),
                                    .receivedAt = receivedAt,
                                }}},
                });
                if (!importRequest.has_value())
                    co_return invalid(i18n("Unable to encode source Email/import request."));
                javelin::jmap::api::RequestBuilder importBuilder;
                importBuilder.useCore().useMail();
                const auto importHandle =
                    importBuilder.call(*importRequest, "history-source-import");
                bool dispatched = false;
                auto importCalled = co_await caller.call(context.requestContext, importBuilder, {},
                                                         [&dispatched] { dispatched = true; });
                if (!std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(importCalled))
                {
                    const auto error = callerError(importCalled);
                    if (dispatched)
                    {
                        if (const auto journalError =
                                setPhase(m_databaseConnection, *record, QStringLiteral("creating"),
                                         QStringLiteral("unknown"), error.message))
                            co_return *journalError;
                        record->phase = QStringLiteral("unknown");
                        co_return ambiguous(i18n("Source restoration may have succeeded, but its "
                                                 "server response was lost."));
                    }
                    if (const auto journalError =
                            setPhase(m_databaseConnection, *record, QStringLiteral("creating"),
                                     QStringLiteral("uploaded"), error.message))
                        co_return *journalError;
                    record->phase = QStringLiteral("uploaded");
                    co_return error;
                }

                const auto importRead =
                    javelin::jmap::api::ResponseReader{
                        std::get<javelin::jmap::api::ResponseEnvelope>(importCalled)}
                        .require(importHandle);
                if (const auto* error =
                        std::get_if<javelin::jmap::api::ResponseReaderError>(&importRead))
                {
                    const auto operationError = javelin::jmap::operationError(*error);
                    if (const auto journalError =
                            setPhase(m_databaseConnection, *record, QStringLiteral("creating"),
                                     QStringLiteral("unknown"), operationError.message))
                        co_return *journalError;
                    record->phase = QStringLiteral("unknown");
                    co_return ambiguous(
                        i18n("Source restoration returned an unaccounted response."));
                }
                const auto& imported =
                    std::get<javelin::jmap::api::EmailImportResponse>(importRead);
                if (imported.accountId != context.account.remoteAccountId)
                {
                    if (const auto journalError =
                            setPhase(m_databaseConnection, *record, QStringLiteral("creating"),
                                     QStringLiteral("unknown"),
                                     i18n("Email/import returned another account.")))
                        co_return *journalError;
                    record->phase = QStringLiteral("unknown");
                    co_return ambiguous(i18n("Source restoration returned the wrong account id."));
                }
                const auto created = imported.created.find(record->creationId);
                if (created != imported.created.end())
                {
                    if (const auto error =
                            setComplete(m_databaseConnection, *record, QStringLiteral("creating"),
                                        created->second.id))
                        co_return *error;
                    record->emailId = created->second.id;
                    record->phase = QStringLiteral("complete");
                    continue;
                }
                const auto rejected = imported.notCreated.find(record->creationId);
                if (rejected != imported.notCreated.end() &&
                    rejected->second.type == "alreadyExists" &&
                    rejected->second.existingId.has_value())
                {
                    auto existing =
                        co_await getAuthoritativeEmails(accountId, {*rejected->second.existingId});
                    if (const auto* error = std::get_if<OperationError>(&existing))
                        co_return *error;
                    const auto& authoritative =
                        std::get<javelin::jmap::AuthoritativeEmails>(existing);
                    const auto found =
                        std::ranges::find(authoritative.emails, *rejected->second.existingId,
                                          &javelin::jmap::domain::Email::id);
                    if (found == authoritative.emails.end())
                        co_return invalid(
                            i18n("The server's existing source Email is inaccessible."));
                    std::vector<std::string> missing;
                    for (const auto& mailboxId : mailboxIds)
                        if (!std::ranges::contains(found->mailboxIds, mailboxId))
                            missing.push_back(mailboxId);
                    if (!missing.empty())
                    {
                        auto applied = co_await applyExactEmailMutation(
                            accountId, javelin::jmap::EmailMailboxMutation{
                                           .emailId = found->id,
                                           .addMailboxIds = std::move(missing),
                                           .removeMailboxIds = {},
                                           .addKeywords = {},
                                           .removeKeywords = {},
                                           .operationGroupId = QUuid::createUuid()
                                                                   .toString(QUuid::WithoutBraces)
                                                                   .toStdString(),
                                           .ifInState = authoritative.state,
                                           .authoritativeMailboxIds = found->mailboxIds,
                                           .authoritativeKeywords = found->keywords,
                                           .destroy = false,
                                       });
                        if (const auto* error = std::get_if<OperationError>(&applied))
                            co_return *error;
                        const auto& summary =
                            std::get<javelin::jmap::SubmittedEmailMutations>(applied);
                        if (summary.updatedEmailCount != 1 || summary.failedEmailCount != 0)
                            co_return invalid(
                                i18n("The existing source Email could not be restored "
                                     "to its original mailboxes."));
                    }
                    if (const auto error = setComplete(m_databaseConnection, *record,
                                                       QStringLiteral("creating"), found->id))
                        co_return *error;
                    record->emailId = found->id;
                    record->phase = QStringLiteral("complete");
                    continue;
                }
                if (rejected != imported.notCreated.end())
                {
                    const auto operationError = rejectedImport(rejected->second);
                    if (const auto journalError =
                            setPhase(m_databaseConnection, *record, QStringLiteral("creating"),
                                     QStringLiteral("uploaded"), operationError.message))
                        co_return *journalError;
                    record->phase = QStringLiteral("uploaded");
                    co_return operationError;
                }

                if (const auto error =
                        setPhase(m_databaseConnection, *record, QStringLiteral("creating"),
                                 QStringLiteral("unknown"),
                                 i18n("Email/import did not account for creation.")))
                    co_return *error;
                record->phase = QStringLiteral("unknown");
                co_return ambiguous(i18n("Source restoration returned an incomplete result."));
            }

            if (record->phase == QStringLiteral("unknown"))
            {
                if (!record->preState.has_value())
                    co_return ambiguous(
                        i18n("Source restoration is missing its reconciliation state."));
                std::string sinceState = *record->preState;
                std::vector<std::string> createdIds;
                bool complete = false;
                for (int page = 0; page < 4; ++page)
                {
                    const auto changesRequest = javelin::jmap::api::emailChanges({
                        .accountId = context.account.remoteAccountId,
                        .sinceState = sinceState,
                        .maxChanges = std::optional<std::uint64_t>{100},
                    });
                    if (!changesRequest.has_value())
                        co_return invalid(i18n("Unable to encode source Email/changes request."));
                    javelin::jmap::api::RequestBuilder changesBuilder;
                    changesBuilder.useCore().useMail();
                    const auto changesHandle =
                        changesBuilder.call(*changesRequest, "history-source-changes");
                    auto called = co_await caller.call(context.requestContext, changesBuilder);
                    if (!std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(called))
                        co_return callerError(called);
                    const auto read =
                        javelin::jmap::api::ResponseReader{
                            std::get<javelin::jmap::api::ResponseEnvelope>(called)}
                            .require(changesHandle);
                    if (const auto* error =
                            std::get_if<javelin::jmap::api::ResponseReaderError>(&read))
                        co_return javelin::jmap::operationError(*error);
                    const auto& changes = std::get<javelin::jmap::api::EmailChangesResponse>(read);
                    if (changes.accountId != context.account.remoteAccountId ||
                        changes.oldState != sinceState || changes.newState.empty())
                        co_return ambiguous(
                            i18n("Source Email/changes could not be reconciled safely."));
                    createdIds.insert(createdIds.end(), changes.created.begin(),
                                      changes.created.end());
                    if (!changes.hasMoreChanges)
                    {
                        complete = true;
                        break;
                    }
                    if (changes.newState == sinceState)
                        co_return ambiguous(
                            i18n("Source Email/changes did not advance its state."));
                    sinceState = changes.newState;
                }
                if (!complete)
                    co_return ambiguous(
                        i18n("Too many source changes occurred to reconcile Undo."));

                std::ranges::sort(createdIds);
                createdIds.erase(std::unique(createdIds.begin(), createdIds.end()),
                                 createdIds.end());
                if (createdIds.empty())
                {
                    if (const auto error =
                            setPhase(m_databaseConnection, *record, QStringLiteral("unknown"),
                                     QStringLiteral("uploaded")))
                        co_return *error;
                    record->phase = QStringLiteral("uploaded");
                    continue;
                }
                if (messageIds.empty() || !context.requestContext.requestLimits.has_value() ||
                    createdIds.size() > static_cast<std::size_t>(
                                            context.requestContext.requestLimits->maxObjectsInGet))
                    co_return ambiguous(i18n("Source restoration may have created a message, but "
                                             "it cannot be correlated uniquely."));

                const auto candidatesRequest = javelin::jmap::api::emailGet({
                    .accountId = context.account.remoteAccountId,
                    .ids = createdIds,
                    .idsReference = std::nullopt,
                    .properties = emailProperties(),
                });
                if (!candidatesRequest.has_value())
                    co_return invalid(i18n("Unable to encode source candidate Email/get request."));
                javelin::jmap::api::RequestBuilder candidatesBuilder;
                candidatesBuilder.useCore().useMail();
                const auto candidatesHandle =
                    candidatesBuilder.call(*candidatesRequest, "history-source-candidates");
                auto called = co_await caller.call(context.requestContext, candidatesBuilder);
                if (!std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(called))
                    co_return callerError(called);
                const auto read =
                    javelin::jmap::api::ResponseReader{
                        std::get<javelin::jmap::api::ResponseEnvelope>(called)}
                        .require(candidatesHandle);
                if (const auto* error = std::get_if<javelin::jmap::api::ResponseReaderError>(&read))
                    co_return javelin::jmap::operationError(*error);
                const auto& candidates = std::get<javelin::jmap::api::EmailGetResponse>(read);
                if (candidates.accountId != context.account.remoteAccountId)
                    co_return ambiguous(
                        i18n("Source candidate Email/get returned another account."));

                auto expectedMessageIds = normalized(messageIds);
                const javelin::jmap::domain::Email* matched = nullptr;
                for (const auto& candidate : candidates.list)
                {
                    auto candidateMessageIds = normalized(candidate.messageId);
                    const bool receivedMatches =
                        !receivedAt.has_value() || candidate.receivedAt == *receivedAt;
                    const bool matches = candidateMessageIds == expectedMessageIds &&
                                         receivedMatches && candidate.size == sourceSize &&
                                         containsAll(candidate.mailboxIds, mailboxIds);
                    if (!matches)
                        continue;
                    if (matched != nullptr)
                        co_return ambiguous(
                            i18n("More than one source recreation candidate matches."));
                    matched = &candidate;
                }
                if (matched == nullptr)
                    co_return ambiguous(i18n("The source recreation candidate cannot be identified "
                                             "uniquely enough to continue."));
                if (const auto error = setComplete(m_databaseConnection, *record,
                                                   QStringLiteral("unknown"), matched->id))
                    co_return *error;
                record->emailId = matched->id;
                record->phase = QStringLiteral("complete");
                continue;
            }

            co_return invalid(i18n("The source recreation journal has an invalid phase."));
        }

        co_return ambiguous(i18n("The source recreation did not reach a stable state."));
    }

    QCoro::Task<RetainedMailTransferSourceResult>
    MailTransferHistoryService::retainSourceForHistory(QString historyEntryId,
                                                       std::string accountId, std::string emailId)
    {
        if (historyEntryId.isEmpty() || accountId.empty() || emailId.empty())
            co_return invalid(i18n("The source retention request is incomplete."));
        const auto settings = m_connectionProvider.connectionSettingsFor(accountId);
        if (!settings.has_value())
            co_return invalid(i18n("Connection settings are unavailable for this mail account."));

        javelin::jmap::MessageContentClient content{m_databaseConnection, m_resourceTransport};
        auto refreshed = co_await content.refresh(liveSettings(*settings), accountId, emailId);
        if (const auto* error = std::get_if<OperationError>(&refreshed))
            co_return *error;
        if (const auto* unavailable =
                std::get_if<javelin::jmap::MessageContentUnavailable>(&refreshed))
        {
            co_return OperationError{
                .code = OperationErrorCode::NotFound,
                .message = unavailable->message,
            };
        }

        javelin::jmap::cache::RawMessageSourceRepository sources{m_databaseConnection};
        auto referenceResult = sources.findReference(accountId, emailId);
        if (const auto* error = std::get_if<DatabaseError>(&referenceResult))
            co_return javelin::jmap::operationError(*error);
        const auto& reference =
            std::get<std::optional<javelin::jmap::cache::RawMessageSourceReference>>(
                referenceResult);
        if (!reference.has_value())
            co_return invalid(i18n("The exact raw source is unavailable after message refresh."));

        QSqlQuery pin{m_databaseConnection.database()};
        pin.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO mail_vault_pins(owner_kind,owner_id,content_hash) VALUES("
            "'history_entry',:owner_id,:content_hash)"));
        pin.bindValue(QStringLiteral(":owner_id"), historyEntryId);
        pin.bindValue(QStringLiteral(":content_hash"),
                      QString::fromStdString(reference->object.contentHash));
        {
            const javelin::jmap::cache::DatabaseWriteScope writeScope{m_databaseConnection};
            if (!pin.exec())
                co_return dbError(QStringLiteral("Retain source MIME for transfer history"), pin);
        }
        if (const auto error = verifyHistoryPin(m_databaseConnection, historyEntryId,
                                                reference->object.contentHash))
            co_return *error;
        co_return reference->object.contentHash;
    }

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ == 16
#pragma GCC diagnostic push
    // GCC 16 emits a false -Wmaybe-uninitialized from std::string destruction in this coroutine's
    // generated frame at -O3. Keep the suppression local so later compilers re-check the function.
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
    QCoro::Task<RedoneMailTransferItemResult> MailTransferHistoryService::redoMissingDestination(
        QString historyEntryId, const MailTransferHistoryOperation operation,
        std::string sourceAccountId, std::string destinationAccountId,
        std::string destinationMailboxId, MailTransferItemHistory item)
    {
        if (historyEntryId.isEmpty() || sourceAccountId.empty() || destinationAccountId.empty() ||
            destinationMailboxId.empty() || !item.currentSourceEmailId.has_value())
            co_return invalid(i18n("The mail transfer Redo request is incomplete."));
        const std::string sourceEmailId = *item.currentSourceEmailId;

        auto redoResult =
            findRedoOperation(m_databaseConnection, historyEntryId, item.redoGeneration,
                              sourceEmailId, destinationAccountId, destinationMailboxId, operation);
        if (const auto* error = std::get_if<OperationError>(&redoResult))
            co_return *error;
        auto redoOperationId = std::get<std::optional<std::string>>(std::move(redoResult));
        if (!redoOperationId.has_value())
        {
            // Only a new generation needs source preflight. Once a durable Redo transfer exists,
            // retries must resume that journal even if its Move has already removed the source.
            auto sourceResult = co_await getAuthoritativeEmails(sourceAccountId, {sourceEmailId});
            if (const auto* error = std::get_if<OperationError>(&sourceResult))
                co_return *error;
            const auto& source = std::get<javelin::jmap::AuthoritativeEmails>(sourceResult);
            const auto sourceFound =
                std::ranges::find(source.emails, sourceEmailId, &javelin::jmap::domain::Email::id);
            if (sourceFound == source.emails.end())
                co_return invalid(i18n("The source message is no longer available for Redo."));
            if (operation == MailTransferHistoryOperation::Move &&
                !containsAll(sourceFound->mailboxIds, item.sourceRemovedMailboxIds))
            {
                co_return invalid(i18n("The source message no longer has the mailbox memberships "
                                       "required to repeat this move."));
            }
            auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
                m_databaseConnection, QStringLiteral("Refresh mail transfer Redo source"));
            if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
                co_return javelin::jmap::operationError(*error);
            auto transaction =
                std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));
            javelin::jmap::cache::EmailRepository sourceEmails{m_databaseConnection};
            if (const auto error =
                    sourceEmails.upsertMany(transaction, sourceAccountId, {*sourceFound}))
                co_return javelin::jmap::operationError(*error);
            javelin::jmap::cache::MailboxWindowRepository mailboxWindows{m_databaseConnection};
            for (const auto& mailboxId : sourceFound->mailboxIds)
                if (const auto error =
                        mailboxWindows.invalidateMailbox(transaction, sourceAccountId, mailboxId))
                    co_return javelin::jmap::operationError(*error);
            javelin::jmap::cache::SearchWindowRepository searchWindows{m_databaseConnection};
            if (const auto error = searchWindows.invalidateAccount(transaction, sourceAccountId))
                co_return javelin::jmap::operationError(*error);
            if (const auto error = transaction.commit())
                co_return javelin::jmap::operationError(*error);

            javelin::app::MailTransferApplicationService preparation{m_databaseConnection};
            std::vector<javelin::app::MailTransferSourceCleanupOverride> cleanupOverrides;
            if (operation == MailTransferHistoryOperation::Move)
            {
                cleanupOverrides.push_back({
                    .emailId = sourceEmailId,
                    .removeMailboxIds = item.sourceRemovedMailboxIds,
                });
            }
            auto prepared = co_await preparation.prepare({
                .intent =
                    {
                        .sourceAccountId = sourceAccountId,
                        .sourceMailboxId = std::nullopt,
                        .destinationAccountId = destinationAccountId,
                        .destinationMailboxId = destinationMailboxId,
                        .operation = operation == MailTransferHistoryOperation::Move
                                         ? javelin::app::MailTransferOperation::Move
                                         : javelin::app::MailTransferOperation::Copy,
                    },
                .selection = {javelin::app::SelectedEmail{.emailId = sourceEmailId}},
                .sourceCleanupOverrides = std::move(cleanupOverrides),
            });
            if (const auto* error = std::get_if<OperationError>(&prepared))
                co_return *error;
            redoOperationId = std::get<javelin::app::PreparedMailTransfer>(prepared).operationId;
            if (const auto error = recordRedoOperation(
                    m_databaseConnection, historyEntryId, item.redoGeneration, sourceEmailId,
                    destinationAccountId, destinationMailboxId, operation, *redoOperationId))
                co_return *error;
            auto persistedRedo = findRedoOperation(
                m_databaseConnection, historyEntryId, item.redoGeneration, sourceEmailId,
                destinationAccountId, destinationMailboxId, operation);
            if (const auto* error = std::get_if<OperationError>(&persistedRedo))
                co_return *error;
            const auto& persistedOperationId = std::get<std::optional<std::string>>(persistedRedo);
            if (!persistedOperationId.has_value())
                co_return invalid(i18n("The durable Redo transfer mapping could not be created."));
            redoOperationId = *persistedOperationId;
            // Mark this transfer as belonging to the existing history entry. There is deliberately
            // no history coordinator on the executor below, so Redo cannot publish a second entry.
            javelin::app::MailTransferRepository transfers{m_databaseConnection};
            const auto marked = transfers.markHistoryPublished(*redoOperationId, historyEntryId);
            if (const auto* error = std::get_if<DatabaseError>(&marked))
                co_return javelin::jmap::operationError(*error);
            if (!std::get<bool>(marked))
                co_return invalid(i18n("The Redo transfer history marker changed unexpectedly."));
        }

        javelin::jmap::MessageContentClient content{m_databaseConnection, m_resourceTransport};
        javelin::app::MailTransferExecutor executor{m_databaseConnection, m_resourceTransport,
                                                    m_methodTransport,    content,
                                                    m_connectionProvider, nullptr};
        auto executed = co_await executor.advance(*redoOperationId);
        if (const auto* error = std::get_if<OperationError>(&executed))
            co_return *error;
        const auto& summary = std::get<javelin::app::MailTransferExecutionSummary>(executed);
        if (summary.status == javelin::app::MailTransferStatus::BlockedUnknown)
            co_return ambiguous(i18n("The repeated mail transfer has an unknown server outcome."));
        if (summary.status == javelin::app::MailTransferStatus::Partial)
        {
            co_return OperationError{
                .code = OperationErrorCode::Conflict,
                .message = i18n("The repeated mail transfer completed only partially."),
                .protocolType = std::optional<std::string>{"partialOutcome"},
            };
        }
        if (summary.status != javelin::app::MailTransferStatus::Complete)
            co_return invalid(i18n("The repeated mail transfer did not reach a complete state."));

        javelin::app::MailTransferRepository transfers{m_databaseConnection};
        const auto itemsResult = transfers.listItems(*redoOperationId);
        if (const auto* error = std::get_if<DatabaseError>(&itemsResult))
            co_return javelin::jmap::operationError(*error);
        const auto& transferItems =
            std::get<std::vector<javelin::app::MailTransferItemRecord>>(itemsResult);
        if (transferItems.size() != 1 ||
            transferItems.front().phase != javelin::app::MailTransferItemPhase::Complete ||
            !transferItems.front().destinationEmailId.has_value())
            co_return invalid(i18n("The repeated transfer journal is incomplete."));
        const auto& transferItem = transferItems.front();

        if (transferItem.sourceDestroy)
        {
            if (!transferItem.rawContentHash.has_value())
                co_return invalid(
                    i18n("The repeated destructive move did not retain its raw source."));
            const auto reassigned = transfers.reassignSourcePin(
                transferItem.itemId, "history_entry", historyEntryId.toStdString());
            if (const auto* error = std::get_if<DatabaseError>(&reassigned))
                co_return javelin::jmap::operationError(*error);
            if (!std::get<bool>(reassigned))
                co_return invalid(i18n("The repeated move could not retain its source for Undo."));
        }

        javelin::jmap::cache::EmailRepository emails{m_databaseConnection};
        const auto destinationResult =
            emails.find(destinationAccountId, *transferItem.destinationEmailId);
        if (const auto* error = std::get_if<DatabaseError>(&destinationResult))
            co_return javelin::jmap::operationError(*error);
        const auto& destination =
            std::get<std::optional<javelin::jmap::domain::Email>>(destinationResult);
        if (!destination.has_value())
            co_return invalid(
                i18n("The repeated transfer destination is not materialized locally."));

        co_return MailTransferItemHistory{
            .currentSourceEmailId = transferItem.sourceDestroy
                                        ? std::nullopt
                                        : std::optional<std::string>{transferItem.sourceEmailId},
            .originalSourceMailboxIds = transferItem.sourceMailboxIds,
            .sourceKeywords = transferItem.sourceKeywords,
            .sourceMessageIds = transferItem.sourceMessageIds,
            .sourceReceivedAt = transferItem.sourceReceivedAt,
            .sourceSize = transferItem.sourceSize,
            .sourceRemovedMailboxIds = transferItem.sourceRemoveMailboxIds,
            .sourceDestroyed = transferItem.sourceDestroy,
            .rawContentHash =
                transferItem.sourceDestroy ? transferItem.rawContentHash : item.rawContentHash,
            .currentDestinationEmailId = transferItem.destinationEmailId,
            .destinationReusedExisting = transferItem.reusedExisting,
            .destinationPriorMailboxIds =
                transferItem.destinationPriorMailboxIds.value_or(std::vector<std::string>{}),
            .destinationMailboxIds = destination->mailboxIds,
            .destinationKeywords = destination->keywords,
            .redoGeneration = item.redoGeneration,
        };
    }
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ == 16
#pragma GCC diagnostic pop
#endif

} // namespace javelin::app::undo
