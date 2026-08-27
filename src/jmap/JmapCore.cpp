#include "jmap/AccountBootstrapClient.h"
#include "jmap/MessageContentClient.h"
#include "jmap/api/SessionRefreshClient.h"
#include "jmap/query/MailQueryClient.h"
#include "jmap/query/MailQueryMaterializer.h"
#include "jmap/sync/EmailMutationEngine.h"
#include "jmap/sync/MailboxMutationEngine.h"

#include "jmap/api/Error.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MailMethods.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/MethodEnvelope.h"
#include "jmap/api/PatchObject.h"
#include "jmap/api/RequestBuilder.h"
#include "jmap/api/ResponseReader.h"
#include "jmap/api/SessionClient.h"
#include "jmap/api/Transport.h"
#include "jmap/auth/AccessTokenResolver.h"
#include "jmap/auth/Auth.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailVault.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/MessageContentTypes.h"
#include "jmap/cache/MessageSummaryReadRepository.h"
#include "jmap/cache/MimeMessageParser.h"
#include "jmap/cache/RawMessageSourceRepository.h"
#include "jmap/cache/SearchWindowRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/cache/ThreadRepository.h"
#include "jmap/sync/ConsistencyDomain.h"
#include "jmap/sync/EmailMutationJournal.h"
#include "jmap/sync/EmailMutationQueue.h"
#include "jmap/sync/MailboxMutationJournal.h"
#include "jmap/sync/MailboxQueryDescriptor.h"
#include "jmap/sync/MailboxRefreshExecutor.h"

#include <QCoroFuture>

#include <QDebug>
#include <QFile>
#include <QLoggingCategory>
#include <QScopeGuard>
#include <QSqlError>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QtConcurrentRun>
#include <algorithm>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace javelin::jmap
{
    Q_LOGGING_CATEGORY(logMessageContent, "jmap.messagecontent", QtInfoMsg)

    struct MailCapabilityContext
    {
        javelin::jmap::cache::DatabaseConnection* databaseConnection = nullptr;
        javelin::jmap::api::AbstractTransport* resourceTransport = nullptr;
        javelin::jmap::api::JmapMethodTransport* methodTransport = nullptr;
        MailQueryClient* queryClient = nullptr;
    };

    namespace
    {

        struct CachedAccountIdentity
        {
            std::string localAccountId;
            std::string remoteAccountId;
        };

        struct DownloadContext
        {
            javelin::jmap::auth::AccountCredentials credentials;
            javelin::jmap::api::Session session;
            std::string remoteAccountId;
            std::string accessToken;
        };

        struct BlobDownloadError
        {
            OperationError error;
            std::optional<int> httpStatus;
        };

        struct MessageSourceStoreResult
        {
            bool stored = false;
            QString error;
        };

        [[nodiscard]] MessageSourceStoreResult
        storeDownloadedMessageSource(const QString& databasePath, const std::string& accountId,
                                     const std::string& emailId, const std::string& blobId,
                                     QString incomingPath)
        {
            javelin::jmap::cache::ThreadConnectionFactory factory({
                .connectionNamePrefix = QStringLiteral("message-source-store"),
                .databasePath = databasePath,
            });
            auto opened = factory.openForCurrentThread(accountId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
            {
                QFile::remove(incomingPath);
                return {.error = error->message};
            }
            auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
            const auto installed = javelin::jmap::cache::MailVault::forDatabase(connection)
                                       .installIncoming(incomingPath);
            if (const auto* error = std::get_if<javelin::jmap::cache::MailVaultError>(&installed))
            {
                QFile::remove(incomingPath);
                return {.error = error->message};
            }
            javelin::jmap::cache::RawMessageSourceRepository sources{connection};
            const auto stored = sources.upsertInstalledIfCurrent(
                accountId, emailId, blobId,
                std::get<javelin::jmap::cache::MailVaultObject>(installed));
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&stored))
                return {.error = error->message};
            return {.stored = std::get<bool>(stored), .error = {}};
        }

        [[nodiscard]] QString encodedTemplateValue(const std::string_view value)
        {
            return QString::fromUtf8(
                QUrl::toPercentEncoding(QString::fromStdString(std::string{value})));
        }

        [[nodiscard]] QUrl buildDownloadUrl(const std::string_view templateUrl,
                                            const std::string_view accountId,
                                            const javelin::jmap::cache::EmailPart& part)
        {
            QString expanded = QString::fromStdString(std::string{templateUrl});
            expanded.replace(QStringLiteral("{accountId}"), encodedTemplateValue(accountId));
            expanded.replace(QStringLiteral("{blobId}"), encodedTemplateValue(*part.blobId));
            expanded.replace(QStringLiteral("{name}"), part.name.has_value()
                                                           ? encodedTemplateValue(*part.name)
                                                           : QStringLiteral(""));
            expanded.replace(QStringLiteral("{type}"), encodedTemplateValue(part.mediaType));
            return QUrl{expanded};
        }

        [[nodiscard]] javelin::jmap::api::HttpRequest
        buildDownloadRequest(const QUrl& url, const std::string& accountId,
                             const std::string& accessToken)
        {
            return javelin::jmap::api::HttpRequest{
                .method = javelin::jmap::api::HttpMethod::Get,
                .url = url,
                .headers =
                    {
                        javelin::jmap::api::HttpHeader{
                            .name = "Authorization",
                            .value = QByteArray{"Bearer "} + QByteArray::fromStdString(accessToken),
                        },
                        javelin::jmap::api::HttpHeader{
                            .name = "Accept",
                            .value = "*/*",
                        },
                    },
                .body = {},
                .authentication =
                    javelin::jmap::api::BearerAuthentication{
                        .accountId = accountId,
                        .accessToken = accessToken,
                    },
                .cancellation = {},
                .dispatched = {},
            };
        }

        [[nodiscard]] javelin::jmap::auth::AccountCredentials
        buildAccountCredentials(const LiveConnectionSettings& settings, std::string accountId)
        {
            return javelin::jmap::auth::AccountCredentials{
                .accountId = std::move(accountId),
                .emailAddress = settings.loginEmail,
                .sessionUrl = settings.sessionUrl,
                .token =
                    {
                        .accessToken = settings.apiKey,
                        .refreshToken = std::nullopt,
                        .expiry = std::nullopt,
                    },
            };
        }

        [[nodiscard]] std::optional<OperationError>
        validateLoginSettings(const LiveConnectionSettings& settings, const bool requireSessionUrl)
        {
            if ((requireSessionUrl && settings.sessionUrl.empty()) || settings.loginEmail.empty() ||
                settings.apiKey.empty())
            {
                return OperationError{
                    .message =
                        requireSessionUrl
                            ? QStringLiteral("Session URL, login email, and API key are required.")
                            : QStringLiteral("Login email and API key are required."),
                };
            }

            return std::nullopt;
        }

        [[nodiscard]] std::variant<javelin::jmap::api::Session, OperationError>
        loadCachedSession(javelin::jmap::cache::DatabaseConnection& connection,
                          const std::string_view accountId)
        {
            javelin::jmap::cache::SessionRepository sessionRepository{connection};
            const auto sessionResult = sessionRepository.load(accountId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&sessionResult))
            {
                return javelin::jmap::operationError(*error);
            }

            const auto& session =
                std::get<std::optional<javelin::jmap::api::Session>>(sessionResult);
            if (!session.has_value())
            {
                return OperationError{
                    .message =
                        QStringLiteral("No cached JMAP session is available for this account."),
                };
            }

            return *session;
        }

        [[nodiscard]] std::variant<CachedAccountIdentity, OperationError>
        resolveCachedAccountIdentity(javelin::jmap::cache::DatabaseConnection& connection,
                                     const std::string_view accountId)
        {
            javelin::jmap::cache::AccountRepository accounts{connection};
            const auto accountResult = accounts.findById(accountId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&accountResult))
            {
                return operationError(*error);
            }

            const auto& account =
                std::get<std::optional<javelin::jmap::cache::CachedAccount>>(accountResult);
            if (!account.has_value() || account->remoteAccountId.empty())
            {
                return OperationError{
                    .code = OperationErrorCode::NotFound,
                    .message = QStringLiteral(
                        "The cached mail account does not have a remote JMAP identity."),
                };
            }

            return CachedAccountIdentity{
                .localAccountId = account->accountId,
                .remoteAccountId = account->remoteAccountId,
            };
        }

        [[nodiscard]] std::variant<std::string, OperationError>
        resolveAccessToken(const javelin::jmap::auth::AccountCredentials& credentials)
        {
            const javelin::jmap::auth::AccessTokenResolver accessTokenResolver;
            const auto tokenResult = accessTokenResolver.resolve(credentials);
            if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&tokenResult))
            {
                return operationError(*error);
            }

            return std::get<javelin::jmap::auth::OAuthToken>(tokenResult).accessToken;
        }

        [[nodiscard]] std::variant<DownloadContext, OperationError>
        prepareDownloadContext(javelin::jmap::cache::DatabaseConnection& connection,
                               const LiveConnectionSettings& settings, const std::string& accountId)
        {
            if (const auto validationError = validateLoginSettings(settings, true))
            {
                return *validationError;
            }

            const auto identityResult = resolveCachedAccountIdentity(connection, accountId);
            if (const auto* error = std::get_if<OperationError>(&identityResult))
                return *error;
            const auto& identity = std::get<CachedAccountIdentity>(identityResult);

            auto credentials = buildAccountCredentials(settings, identity.localAccountId);
            const auto sessionResult = loadCachedSession(connection, identity.localAccountId);
            if (const auto* error = std::get_if<OperationError>(&sessionResult))
            {
                return *error;
            }

            const auto tokenResult = resolveAccessToken(credentials);
            if (const auto* error = std::get_if<OperationError>(&tokenResult))
            {
                return *error;
            }

            return DownloadContext{
                .credentials = std::move(credentials),
                .session = std::get<javelin::jmap::api::Session>(sessionResult),
                .remoteAccountId = identity.remoteAccountId,
                .accessToken = std::get<std::string>(tokenResult),
            };
        }

        [[nodiscard]] QCoro::Task<std::variant<std::uint64_t, BlobDownloadError>>
        downloadBlobToFile(javelin::jmap::api::AbstractTransport& transport,
                           std::string downloadUrlTemplate, std::string remoteAccountId,
                           std::string authenticationAccountId,
                           javelin::jmap::cache::EmailPart part, std::string accessToken,
                           QString filePath, QString failurePrefix)
        {
            const auto transportResult = co_await transport.sendToFile(
                buildDownloadRequest(buildDownloadUrl(downloadUrlTemplate, remoteAccountId, part),
                                     authenticationAccountId, accessToken),
                std::move(filePath));
            if (const auto* error =
                    std::get_if<javelin::jmap::api::TransportError>(&transportResult))
            {
                co_return BlobDownloadError{
                    .error = operationError(*error),
                    .httpStatus = error->httpStatus,
                };
            }

            const auto& response = std::get<javelin::jmap::api::HttpFileResponse>(transportResult);
            if (response.statusCode < 200 || response.statusCode >= 300)
            {
                co_return BlobDownloadError{
                    .error =
                        OperationError{
                            .message = QStringLiteral("%1 failed with HTTP status %2.")
                                           .arg(failurePrefix)
                                           .arg(response.statusCode),
                        },
                    .httpStatus = response.statusCode,
                };
            }

            co_return response.size;
        }

        [[nodiscard]] std::variant<javelin::jmap::domain::Email, OperationError>
        findEmailForDownload(javelin::jmap::cache::DatabaseConnection& connection,
                             const std::string_view accountId, const std::string_view emailId)
        {
            javelin::jmap::cache::EmailRepository emailRepository{connection};
            const auto emailResult = emailRepository.find(accountId, emailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&emailResult))
            {
                return javelin::jmap::operationError(*error);
            }

            const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(emailResult);
            if (!email.has_value())
            {
                return OperationError{
                    .message = QStringLiteral("The selected message is not cached locally."),
                };
            }

            if (email->blobId.empty())
            {
                return OperationError{
                    .message = QStringLiteral("The selected message does not expose a blob id."),
                };
            }

            return *email;
        }

        [[nodiscard]] std::vector<javelin::jmap::sync::EmailMutationRecord>
        activeEmailMutations(const std::vector<javelin::jmap::sync::EmailMutationRecord>& actions)
        {
            std::vector<javelin::jmap::sync::EmailMutationRecord> filtered;
            filtered.reserve(actions.size());
            for (const auto& action : actions)
            {
                if (javelin::jmap::sync::projectsOptimistically(action.status))
                {
                    filtered.push_back(action);
                }
            }
            return filtered;
        }

        using EmailMutationRecords = std::vector<javelin::jmap::sync::EmailMutationRecord>;
        using EmailMutationRecordsResult = std::variant<EmailMutationRecords, OperationError>;

        [[nodiscard]] EmailMutationRecordsResult
        loadEmailMutationRecords(javelin::jmap::sync::EmailMutationJournal& journal,
                                 const std::string_view accountId, const std::string_view emailId)
        {
            const auto result = journal.listForEmail(accountId, emailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                return javelin::jmap::operationError(*error);
            return std::get<EmailMutationRecords>(result);
        }

        [[nodiscard]] javelin::jmap::domain::Email
        reprojectActiveEmailMutations(javelin::jmap::domain::Email baseEmail,
                                      const EmailMutationRecords& records)
        {
            return javelin::jmap::sync::projectEmailMutations(baseEmail,
                                                              activeEmailMutations(records));
        }

        [[nodiscard]] std::optional<OperationError> compactSettledEmailMutations(
            javelin::jmap::sync::MutationProjectionTransaction& transaction,
            const EmailMutationRecords& records)
        {
            const bool unresolved = std::ranges::any_of(
                records,
                [](const auto& record)
                {
                    return record.status == javelin::jmap::sync::MutationStatus::Pending ||
                           record.status == javelin::jmap::sync::MutationStatus::InFlight ||
                           record.status == javelin::jmap::sync::MutationStatus::Unknown;
                });
            if (unresolved)
                return std::nullopt;
            if (const auto error = transaction.retireTerminal())
                return javelin::jmap::operationError(*error);
            return std::nullopt;
        }

        [[nodiscard]] javelin::jmap::api::ApiRequestContext
        buildApiRequestContext(const LiveConnectionSettings& settings, std::string accountId,
                               const javelin::jmap::api::Session& session)
        {
            return javelin::jmap::api::ApiRequestContext{
                .credentials = buildAccountCredentials(settings, std::move(accountId)),
                .apiUrl = session.apiUrl,
                .requestLimits = javelin::jmap::api::coreRequestLimits(session),
            };
        }

        [[nodiscard]] QCoro::Task<std::variant<CollapsedQueryPage, OperationError>>
        performCollapsedQueryPage(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                                  javelin::jmap::api::JmapMethodTransport& methodTransport,
                                  LiveConnectionSettings settings, std::string accountId,
                                  javelin::jmap::api::EmailQueryFilter filter,
                                  const std::size_t offset, const std::size_t limit,
                                  javelin::jmap::query::EmailListSort sort,
                                  std::optional<std::string> anchor,
                                  const std::int64_t anchorOffset,
                                  std::function<void(const QString&)> reportProgress)
        {
            if (const auto validationError = validateLoginSettings(settings, true))
            {
                co_return *validationError;
            }

            const auto identityResult = resolveCachedAccountIdentity(databaseConnection, accountId);
            if (const auto* error = std::get_if<OperationError>(&identityResult))
                co_return *error;
            const auto& identity = std::get<CachedAccountIdentity>(identityResult);

            const auto sessionResult =
                loadCachedSession(databaseConnection, identity.localAccountId);
            if (const auto* error = std::get_if<OperationError>(&sessionResult))
            {
                co_return *error;
            }
            const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);

            javelin::jmap::api::MethodCaller methodCaller{methodTransport};
            const auto apiRequestContext =
                buildApiRequestContext(settings, identity.localAccountId, session);
            const auto effectiveLimit = std::min<std::size_t>(
                limit, static_cast<std::size_t>(apiRequestContext.requestLimits->maxObjectsInGet));

            javelin::jmap::api::RequestBuilder builder;
            builder.useCore().useMail();

            const auto queryRequest = javelin::jmap::api::emailQuery({
                .accountId = identity.remoteAccountId,
                .filter = filter,
                .sort = {javelin::jmap::query::toEmailQuerySort(sort)},
                .position = anchor.has_value()
                                ? std::nullopt
                                : std::optional<std::uint64_t>{static_cast<std::uint64_t>(offset)},
                .anchor = std::move(anchor),
                .anchorOffset = anchorOffset,
                .limit = static_cast<std::uint64_t>(effectiveLimit),
                .collapseThreads = true,
                .calculateTotal = true,
            });
            if (!queryRequest.has_value())
            {
                co_return OperationError{
                    .message = QStringLiteral("Failed to encode the Email/query request."),
                };
            }
            const auto queryHandle = builder.call(*queryRequest, "page-query");

            const auto representativeRequest = javelin::jmap::api::emailGet(
                javelin::jmap::api::getRequestFrom(identity.remoteAccountId, queryHandle, "/ids"));
            if (!representativeRequest.has_value())
            {
                co_return OperationError{
                    .message =
                        QStringLiteral("Failed to encode the representative Email/get request."),
                };
            }
            const auto representativeHandle =
                builder.call(*representativeRequest, "page-representatives-get");

            const auto envelopeResult = co_await methodCaller.call(apiRequestContext, builder);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::TransportError>(&envelopeResult))
            {
                co_return operationError(*error);
            }
            if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&envelopeResult))
            {
                co_return operationError(*error);
            }
            if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&envelopeResult))
            {
                co_return operationError(*error);
            }

            const auto& envelope = std::get<javelin::jmap::api::ResponseEnvelope>(envelopeResult);
            const javelin::jmap::api::ResponseReader reader{envelope};

            const auto queryResult = reader.require(queryHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&queryResult))
            {
                co_return operationError(*error);
            }
            const auto& parsedQuery = std::get<javelin::jmap::api::EmailQueryResponse>(queryResult);
            if (const auto accountError = validateResponseAccountId(
                    identity.remoteAccountId, parsedQuery.accountId, u"Email/query"))
                co_return *accountError;
            if (reportProgress)
            {
                reportProgress(QStringLiteral("Fetched %1 matching conversations.")
                                   .arg(static_cast<qulonglong>(parsedQuery.ids.size())));
            }

            const auto representativeResult = reader.require(representativeHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&representativeResult))
            {
                co_return operationError(*error);
            }
            const auto& parsedRepresentatives =
                std::get<javelin::jmap::api::EmailGetResponse>(representativeResult);
            if (const auto accountError = validateResponseAccountId(
                    identity.remoteAccountId, parsedRepresentatives.accountId, u"Email/get"))
                co_return *accountError;

            std::unordered_map<std::string, const javelin::jmap::domain::Email*>
                representativesById;
            representativesById.reserve(parsedRepresentatives.list.size());
            for (const auto& email : parsedRepresentatives.list)
            {
                representativesById.emplace(email.id, &email);
            }

            std::vector<javelin::jmap::domain::Email> representatives;
            representatives.reserve(parsedQuery.ids.size());
            for (const auto& representativeId : parsedQuery.ids)
            {
                const auto representativeIt = representativesById.find(representativeId);
                if (representativeIt == representativesById.end())
                    continue;
                representatives.push_back(*representativeIt->second);
            }
            if (representatives.size() != parsedQuery.ids.size())
            {
                co_return OperationError{
                    .message = QStringLiteral(
                                   "Email/get omitted %1 representatives from the query window.")
                                   .arg(static_cast<qulonglong>(parsedQuery.ids.size() -
                                                                representatives.size())),
                };
            }

            co_return CollapsedQueryPage{
                .representativeCount = representatives.size(),
                .position = static_cast<std::size_t>(parsedQuery.position),
                .returnedLimit =
                    static_cast<std::size_t>(parsedQuery.limit.value_or(effectiveLimit)),
                .total =
                    parsedQuery.total.has_value()
                        ? std::optional<std::size_t>{static_cast<std::size_t>(*parsedQuery.total)}
                        : std::nullopt,
                .queryState = parsedQuery.queryState,
                .emailState = parsedRepresentatives.state,
                .representativeIds = parsedQuery.ids,
                .representatives = std::move(representatives),
            };
        }

        [[nodiscard]] OperationError
        mailboxSetError(const javelin::jmap::api::MailboxSetError& error, const QString& fallback)
        {
            auto code = OperationErrorCode::ServerFailure;
            if (error.type == "forbidden")
                code = OperationErrorCode::PermissionDenied;
            else if (error.type == "invalidArguments" || error.type == "invalidProperties")
                code = OperationErrorCode::InvalidUserInput;
            else if (error.type == "notFound")
                code = OperationErrorCode::NotFound;
            else if (error.type == "stateMismatch")
                code = OperationErrorCode::Conflict;
            else if (error.type == "mailboxHasEmail" || error.type == "mailboxHasChild")
                code = OperationErrorCode::PreconditionFailed;
            return OperationError{
                .code = code,
                .message = error.description.has_value()
                               ? QString::fromStdString(*error.description)
                               : fallback,
                .protocolType = error.type,
            };
        }

        [[nodiscard]] QCoro::Task<
            std::variant<javelin::jmap::api::MailboxSetResponse, OperationError>>
        submitMailboxSubscriptionMutation(
            javelin::jmap::api::JmapMethodTransport& transport,
            const LiveConnectionSettings& settings, const std::string& accountId,
            std::string remoteAccountId, const javelin::jmap::api::Session& session,
            javelin::jmap::sync::MailboxMutationJournal& journal,
            const javelin::jmap::sync::MailboxSubscriptionMutationRecord& mutation)
        {
            if (const auto error =
                    journal.transition(mutation, javelin::jmap::sync::MutationStatus::InFlight))
                co_return operationError(*error);

            const auto request = javelin::jmap::api::mailboxSet({
                .accountId = remoteAccountId,
                .ifInState = mutation.baseState,
                .create = {},
                .update = {{mutation.mailboxId, {.isSubscribed = mutation.afterSubscribed}}},
                .destroy = {},
                .onDestroyRemoveEmails = false,
            });
            if (!request.has_value())
            {
                if (const auto error = journal.reject(mutation))
                    co_return operationError(*error);
                co_return OperationError{
                    .code = OperationErrorCode::InvalidRequest,
                    .message = QStringLiteral("Failed to encode Mailbox/set."),
                };
            }

            javelin::jmap::api::RequestBuilder builder;
            builder.useCore().useMail();
            const auto handle = builder.call(*request, "mailbox-subscription-set");
            javelin::jmap::api::MethodCaller caller{transport};
            bool dispatched = false;
            auto result = co_await caller.call(buildApiRequestContext(settings, accountId, session),
                                               builder, {}, [&dispatched] { dispatched = true; });
            const auto settleCallError = [&](const OperationError& callError)
                -> std::optional<javelin::jmap::cache::DatabaseError>
            {
                const bool deterministic = !dispatched || callError.protocolType.has_value();
                return deterministic
                           ? journal.reject(mutation, std::nullopt, callError.protocolType)
                           : journal.transition(mutation,
                                                javelin::jmap::sync::MutationStatus::Unknown);
            };
            if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&result))
            {
                const auto callError = operationError(*error);
                if (const auto cacheError = settleCallError(callError))
                    co_return operationError(*cacheError);
                co_return callError;
            }
            if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&result))
            {
                const auto callError = operationError(*error);
                if (const auto cacheError = settleCallError(callError))
                    co_return operationError(*cacheError);
                co_return callError;
            }
            if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&result))
            {
                const auto callError = operationError(*error);
                if (const auto cacheError = settleCallError(callError))
                    co_return operationError(*cacheError);
                co_return callError;
            }

            const javelin::jmap::api::ResponseReader reader{
                std::get<javelin::jmap::api::ResponseEnvelope>(result)};
            auto parsed = reader.require(handle);
            if (const auto* error = std::get_if<javelin::jmap::api::ResponseReaderError>(&parsed))
            {
                const auto callError = operationError(*error);
                if (const auto cacheError = settleCallError(callError))
                    co_return operationError(*cacheError);
                co_return callError;
            }
            auto response = std::get<javelin::jmap::api::MailboxSetResponse>(std::move(parsed));
            if (response.accountId != remoteAccountId)
            {
                if (const auto error =
                        journal.transition(mutation, javelin::jmap::sync::MutationStatus::Unknown))
                    co_return operationError(*error);
                co_return OperationError{
                    .code = OperationErrorCode::ProtocolViolation,
                    .message =
                        QStringLiteral("The server returned Mailbox/set for another account."),
                };
            }
            if (const auto rejected = response.notUpdated.find(mutation.mailboxId);
                rejected != response.notUpdated.end())
            {
                if (const auto error =
                        journal.reject(mutation, response.newState, rejected->second.type))
                    co_return operationError(*error);
                co_return mailboxSetError(
                    rejected->second, QStringLiteral("The server rejected the mailbox change."));
            }
            if (std::ranges::find(response.updated, mutation.mailboxId) == response.updated.end() ||
                !response.newState.has_value())
            {
                if (const auto error =
                        journal.transition(mutation, javelin::jmap::sync::MutationStatus::Unknown))
                    co_return operationError(*error);
                co_return OperationError{
                    .code = OperationErrorCode::ProtocolViolation,
                    .message = QStringLiteral("The server did not confirm the mailbox change."),
                };
            }
            if (const auto error = journal.accept(mutation, *response.newState))
            {
                if (const auto transitionError =
                        journal.transition(mutation, javelin::jmap::sync::MutationStatus::Unknown))
                    co_return operationError(*transitionError);
                co_return OperationError{
                    .code = OperationErrorCode::Conflict,
                    .message = QStringLiteral(
                        "Mailbox state advanced while the server change was being confirmed."),
                };
            }
            co_return response;
        }

        [[nodiscard]] QCoro::Task<std::variant<javelin::jmap::domain::Mailbox, OperationError>>
        submitMailboxCreateMutation(
            javelin::jmap::api::JmapMethodTransport& transport,
            const LiveConnectionSettings& settings, const std::string& accountId,
            std::string remoteAccountId, const javelin::jmap::api::Session& session,
            javelin::jmap::sync::MailboxMutationJournal& journal,
            const javelin::jmap::sync::MailboxCreateMutationRecord& mutation)
        {
            if (const auto error =
                    journal.transition(mutation, javelin::jmap::sync::MutationStatus::InFlight))
                co_return operationError(*error);

            const auto request = javelin::jmap::api::mailboxSet({
                .accountId = remoteAccountId,
                .ifInState = mutation.baseState,
                .create = {{mutation.creationId,
                            {.name = mutation.name,
                             .parentId = mutation.parentId,
                             .sortOrder = mutation.sortOrder,
                             .isSubscribed = mutation.isSubscribed}}},
                .update = {},
                .destroy = {},
                .onDestroyRemoveEmails = false,
            });
            if (!request.has_value())
            {
                if (const auto error = journal.reject(mutation))
                    co_return operationError(*error);
                co_return OperationError{
                    .code = OperationErrorCode::InvalidRequest,
                    .message = QStringLiteral("Failed to encode Mailbox/set."),
                };
            }

            javelin::jmap::api::RequestBuilder builder;
            builder.useCore().useMail();
            const auto setHandle = builder.call(*request, "mailbox-create-set");
            const auto createdIdPath =
                "/" + javelin::jmap::api::patchPath("created", mutation.creationId) + "/id";
            const auto getRequest = javelin::jmap::api::mailboxGet(
                javelin::jmap::api::getRequestFrom(remoteAccountId, setHandle, createdIdPath));
            if (!getRequest.has_value())
            {
                if (const auto error = journal.reject(mutation))
                    co_return operationError(*error);
                co_return OperationError{
                    .code = OperationErrorCode::InvalidRequest,
                    .message = QStringLiteral("Failed to encode Mailbox/get."),
                };
            }
            const auto getHandle = builder.call(*getRequest, "mailbox-create-get");

            javelin::jmap::api::MethodCaller caller{transport};
            bool dispatched = false;
            auto result = co_await caller.call(buildApiRequestContext(settings, accountId, session),
                                               builder, {}, [&dispatched] { dispatched = true; });
            const auto settleCallError = [&](const OperationError& callError)
                -> std::optional<javelin::jmap::cache::DatabaseError>
            {
                const bool deterministic = !dispatched || callError.protocolType.has_value();
                return deterministic
                           ? journal.reject(mutation, std::nullopt, callError.protocolType)
                           : journal.transition(mutation,
                                                javelin::jmap::sync::MutationStatus::Unknown);
            };
            if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&result))
            {
                const auto callError = operationError(*error);
                if (const auto cacheError = settleCallError(callError))
                    co_return operationError(*cacheError);
                co_return callError;
            }
            if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&result))
            {
                const auto callError = operationError(*error);
                if (const auto cacheError = settleCallError(callError))
                    co_return operationError(*cacheError);
                co_return callError;
            }
            if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&result))
            {
                const auto callError = operationError(*error);
                if (const auto cacheError = settleCallError(callError))
                    co_return operationError(*cacheError);
                co_return callError;
            }

            const javelin::jmap::api::ResponseReader reader{
                std::get<javelin::jmap::api::ResponseEnvelope>(result)};
            auto parsedSet = reader.require(setHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&parsedSet))
            {
                const auto callError = operationError(*error);
                if (const auto cacheError = settleCallError(callError))
                    co_return operationError(*cacheError);
                co_return callError;
            }
            auto response = std::get<javelin::jmap::api::MailboxSetResponse>(std::move(parsedSet));
            if (response.accountId != remoteAccountId)
            {
                if (const auto error =
                        journal.transition(mutation, javelin::jmap::sync::MutationStatus::Unknown))
                    co_return operationError(*error);
                co_return OperationError{
                    .code = OperationErrorCode::ProtocolViolation,
                    .message =
                        QStringLiteral("The server returned Mailbox/set for another account."),
                };
            }
            if (const auto rejected = response.notCreated.find(mutation.creationId);
                rejected != response.notCreated.end())
            {
                if (const auto error =
                        journal.reject(mutation, response.newState, rejected->second.type))
                    co_return operationError(*error);
                co_return mailboxSetError(rejected->second,
                                          QStringLiteral("The server rejected the new mailbox."));
            }
            const auto created = response.created.find(mutation.creationId);
            if (created == response.created.end() || created->second.empty() ||
                !response.newState.has_value())
            {
                if (const auto error =
                        journal.transition(mutation, javelin::jmap::sync::MutationStatus::Unknown))
                    co_return operationError(*error);
                co_return OperationError{
                    .code = OperationErrorCode::ProtocolViolation,
                    .message = QStringLiteral("The server did not confirm the new mailbox."),
                };
            }

            auto parsedGet = reader.require(getHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&parsedGet))
            {
                if (const auto cacheError =
                        journal.transition(mutation, javelin::jmap::sync::MutationStatus::Unknown))
                    co_return operationError(*cacheError);
                co_return operationError(*error);
            }
            auto fetched = std::get<javelin::jmap::api::MailboxGetResponse>(std::move(parsedGet));
            if (fetched.accountId != remoteAccountId)
            {
                if (const auto error =
                        journal.transition(mutation, javelin::jmap::sync::MutationStatus::Unknown))
                    co_return operationError(*error);
                co_return OperationError{
                    .code = OperationErrorCode::ProtocolViolation,
                    .message =
                        QStringLiteral("The server returned Mailbox/get for another account."),
                };
            }
            const auto mailbox = std::ranges::find(fetched.list, created->second,
                                                   &javelin::jmap::domain::Mailbox::id);
            if (mailbox == fetched.list.end())
            {
                if (const auto error =
                        journal.transition(mutation, javelin::jmap::sync::MutationStatus::Unknown))
                    co_return operationError(*error);
                co_return OperationError{
                    .code = OperationErrorCode::ProtocolViolation,
                    .message = QStringLiteral("The new mailbox could not be materialized."),
                };
            }
            if (const auto error = journal.accept(mutation, *mailbox, *response.newState))
            {
                if (const auto transitionError =
                        journal.transition(mutation, javelin::jmap::sync::MutationStatus::Unknown))
                    co_return operationError(*transitionError);
                co_return OperationError{
                    .code = OperationErrorCode::Conflict,
                    .message = QStringLiteral(
                        "Mailbox state advanced while the new mailbox was being confirmed."),
                };
            }
            co_return *mailbox;
        }

        [[nodiscard]] QCoro::Task<
            std::variant<javelin::jmap::api::MailboxSetResponse, OperationError>>
        submitMailboxDestroyMutation(
            javelin::jmap::api::JmapMethodTransport& transport,
            const LiveConnectionSettings& settings, const std::string& accountId,
            std::string remoteAccountId, const javelin::jmap::api::Session& session,
            javelin::jmap::sync::MailboxMutationJournal& journal,
            const javelin::jmap::sync::MailboxDestroyMutationRecord& mutation)
        {
            if (const auto error =
                    journal.transition(mutation, javelin::jmap::sync::MutationStatus::InFlight))
                co_return operationError(*error);

            const auto request = javelin::jmap::api::mailboxSet({
                .accountId = remoteAccountId,
                .ifInState = mutation.baseState,
                .create = {},
                .update = {},
                .destroy = {mutation.mailboxId},
                .onDestroyRemoveEmails = false,
            });
            if (!request.has_value())
            {
                if (const auto error = journal.reject(mutation))
                    co_return operationError(*error);
                co_return OperationError{
                    .code = OperationErrorCode::InvalidRequest,
                    .message = QStringLiteral("Failed to encode Mailbox/set."),
                };
            }

            javelin::jmap::api::RequestBuilder builder;
            builder.useCore().useMail();
            const auto handle = builder.call(*request, "mailbox-destroy-set");
            javelin::jmap::api::MethodCaller caller{transport};
            bool dispatched = false;
            auto result = co_await caller.call(buildApiRequestContext(settings, accountId, session),
                                               builder, {}, [&dispatched] { dispatched = true; });
            const auto settleCallError = [&](const OperationError& callError)
                -> std::optional<javelin::jmap::cache::DatabaseError>
            {
                const bool deterministic = !dispatched || callError.protocolType.has_value();
                return deterministic
                           ? journal.reject(mutation, std::nullopt, callError.protocolType)
                           : journal.transition(mutation,
                                                javelin::jmap::sync::MutationStatus::Unknown);
            };
            if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&result))
            {
                const auto callError = operationError(*error);
                if (const auto cacheError = settleCallError(callError))
                    co_return operationError(*cacheError);
                co_return callError;
            }
            if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&result))
            {
                const auto callError = operationError(*error);
                if (const auto cacheError = settleCallError(callError))
                    co_return operationError(*cacheError);
                co_return callError;
            }
            if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&result))
            {
                const auto callError = operationError(*error);
                if (const auto cacheError = settleCallError(callError))
                    co_return operationError(*cacheError);
                co_return callError;
            }

            const javelin::jmap::api::ResponseReader reader{
                std::get<javelin::jmap::api::ResponseEnvelope>(result)};
            auto parsed = reader.require(handle);
            if (const auto* error = std::get_if<javelin::jmap::api::ResponseReaderError>(&parsed))
            {
                const auto callError = operationError(*error);
                if (const auto cacheError = settleCallError(callError))
                    co_return operationError(*cacheError);
                co_return callError;
            }
            auto response = std::get<javelin::jmap::api::MailboxSetResponse>(std::move(parsed));
            if (response.accountId != remoteAccountId)
            {
                if (const auto error =
                        journal.transition(mutation, javelin::jmap::sync::MutationStatus::Unknown))
                    co_return operationError(*error);
                co_return OperationError{
                    .code = OperationErrorCode::ProtocolViolation,
                    .message =
                        QStringLiteral("The server returned Mailbox/set for another account."),
                };
            }
            if (const auto rejected = response.notDestroyed.find(mutation.mailboxId);
                rejected != response.notDestroyed.end())
            {
                if (const auto error =
                        journal.reject(mutation, response.newState, rejected->second.type))
                    co_return operationError(*error);
                const auto fallback =
                    rejected->second.type == "mailboxHasEmail"
                        ? QStringLiteral("Empty the mailbox before deleting it.")
                    : rejected->second.type == "mailboxHasChild"
                        ? QStringLiteral("Delete or move child mailboxes before deleting it.")
                        : QStringLiteral("The server rejected the mailbox deletion.");
                co_return mailboxSetError(rejected->second, fallback);
            }
            if (std::ranges::find(response.destroyed, mutation.mailboxId) ==
                    response.destroyed.end() ||
                !response.newState.has_value())
            {
                if (const auto error =
                        journal.transition(mutation, javelin::jmap::sync::MutationStatus::Unknown))
                    co_return operationError(*error);
                co_return OperationError{
                    .code = OperationErrorCode::ProtocolViolation,
                    .message = QStringLiteral("The server did not confirm the mailbox deletion."),
                };
            }
            if (const auto error = journal.accept(mutation, *response.newState))
            {
                if (const auto transitionError =
                        journal.transition(mutation, javelin::jmap::sync::MutationStatus::Unknown))
                    co_return operationError(*transitionError);
                co_return OperationError{
                    .code = OperationErrorCode::Conflict,
                    .message = QStringLiteral(
                        "Mailbox state advanced while the deletion was being confirmed."),
                };
            }
            co_return response;
        }

        [[nodiscard]] QCoro::Task<
            std::variant<javelin::jmap::api::MailboxGetResponse, OperationError>>
        fetchMailboxForReconciliation(javelin::jmap::api::JmapMethodTransport& transport,
                                      const LiveConnectionSettings& settings,
                                      const std::string& accountId, std::string remoteAccountId,
                                      const javelin::jmap::api::Session& session,
                                      const std::string& mailboxId)
        {
            const auto request = javelin::jmap::api::mailboxGet({
                .accountId = remoteAccountId,
                .ids = std::vector<std::string>{mailboxId},
                .idsReference = std::nullopt,
                .properties = std::nullopt,
            });
            if (!request.has_value())
            {
                co_return OperationError{
                    .code = OperationErrorCode::InvalidRequest,
                    .message = QStringLiteral("Failed to encode Mailbox/get."),
                };
            }
            javelin::jmap::api::RequestBuilder builder;
            builder.useCore().useMail();
            const auto handle = builder.call(*request, "mailbox-mutation-reconcile");
            javelin::jmap::api::MethodCaller caller{transport};
            auto result =
                co_await caller.call(buildApiRequestContext(settings, accountId, session), builder);
            if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&result))
                co_return operationError(*error);
            if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&result))
                co_return operationError(*error);
            if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&result))
                co_return operationError(*error);
            const javelin::jmap::api::ResponseReader reader{
                std::get<javelin::jmap::api::ResponseEnvelope>(result)};
            auto parsed = reader.require(handle);
            if (const auto* error = std::get_if<javelin::jmap::api::ResponseReaderError>(&parsed))
                co_return operationError(*error);
            co_return std::get<javelin::jmap::api::MailboxGetResponse>(std::move(parsed));
        }

        [[nodiscard]] QCoro::Task<
            std::variant<javelin::jmap::api::MailboxGetResponse, OperationError>>
        fetchAllMailboxesForReconciliation(javelin::jmap::api::JmapMethodTransport& transport,
                                           const LiveConnectionSettings& settings,
                                           const std::string& accountId,
                                           std::string remoteAccountId,
                                           const javelin::jmap::api::Session& session)
        {
            const auto request = javelin::jmap::api::mailboxGet({
                .accountId = remoteAccountId,
                .ids = std::nullopt,
                .idsReference = std::nullopt,
                .properties = std::nullopt,
            });
            if (!request.has_value())
            {
                co_return OperationError{
                    .code = OperationErrorCode::InvalidRequest,
                    .message = QStringLiteral("Failed to encode Mailbox/get."),
                };
            }
            javelin::jmap::api::RequestBuilder builder;
            builder.useCore().useMail();
            const auto handle = builder.call(*request, "mailbox-create-reconcile");
            javelin::jmap::api::MethodCaller caller{transport};
            auto result =
                co_await caller.call(buildApiRequestContext(settings, accountId, session), builder);
            if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&result))
                co_return operationError(*error);
            if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&result))
                co_return operationError(*error);
            if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&result))
                co_return operationError(*error);
            const javelin::jmap::api::ResponseReader reader{
                std::get<javelin::jmap::api::ResponseEnvelope>(result)};
            auto parsed = reader.require(handle);
            if (const auto* error = std::get_if<javelin::jmap::api::ResponseReaderError>(&parsed))
                co_return operationError(*error);
            co_return std::get<javelin::jmap::api::MailboxGetResponse>(std::move(parsed));
        }

    } // namespace

    SessionRefreshClient::SessionRefreshClient(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::api::AbstractTransport& resourceTransport)
        : m_impl(std::make_unique<MailCapabilityContext>())
    {
        m_impl->databaseConnection = &databaseConnection;
        m_impl->resourceTransport = &resourceTransport;
    }

    SessionRefreshClient::~SessionRefreshClient() = default;

    AccountBootstrapClient::AccountBootstrapClient(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::api::AbstractTransport& resourceTransport,
        javelin::jmap::api::JmapMethodTransport& methodTransport)
        : m_impl(std::make_unique<MailCapabilityContext>())
    {
        m_impl->databaseConnection = &databaseConnection;
        m_impl->resourceTransport = &resourceTransport;
        m_impl->methodTransport = &methodTransport;
    }

    AccountBootstrapClient::~AccountBootstrapClient() = default;

    MailQueryClient::MailQueryClient(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                                     javelin::jmap::api::JmapMethodTransport& methodTransport)
        : m_impl(std::make_unique<MailCapabilityContext>())
    {
        m_impl->databaseConnection = &databaseConnection;
        m_impl->methodTransport = &methodTransport;
    }

    MailQueryClient::~MailQueryClient() = default;

    MailQueryMaterializer::MailQueryMaterializer(
        javelin::jmap::cache::DatabaseConnection& databaseConnection, MailQueryClient& queryClient)
        : m_impl(std::make_unique<MailCapabilityContext>())
    {
        m_impl->databaseConnection = &databaseConnection;
        m_impl->queryClient = &queryClient;
    }

    MailQueryMaterializer::~MailQueryMaterializer() = default;

    MessageContentClient::MessageContentClient(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::api::AbstractTransport& resourceTransport)
        : m_impl(std::make_unique<MailCapabilityContext>())
    {
        m_impl->databaseConnection = &databaseConnection;
        m_impl->resourceTransport = &resourceTransport;
    }

    MessageContentClient::~MessageContentClient() = default;

    EmailMutationEngine::EmailMutationEngine(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::api::JmapMethodTransport& methodTransport)
        : m_impl(std::make_unique<MailCapabilityContext>())
    {
        m_impl->databaseConnection = &databaseConnection;
        m_impl->methodTransport = &methodTransport;
    }

    EmailMutationEngine::~EmailMutationEngine() = default;

    MailboxMutationEngine::MailboxMutationEngine(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::api::JmapMethodTransport& methodTransport)
        : m_impl(std::make_unique<MailCapabilityContext>())
    {
        m_impl->databaseConnection = &databaseConnection;
        m_impl->methodTransport = &methodTransport;
    }

    MailboxMutationEngine::~MailboxMutationEngine() = default;

    QCoro::Task<SessionRefreshResult>
    SessionRefreshClient::refresh(LiveConnectionSettings settings, std::string connectionId,
                                  std::string ownerAccountId, std::string ownerRemoteAccountId)
    {
        if (m_impl->databaseConnection == nullptr || m_impl->resourceTransport == nullptr)
        {
            co_return OperationError{
                .message = QStringLiteral(
                    "Session discovery is unavailable in this process configuration."),
            };
        }
        if (const auto validationError = validateLoginSettings(settings, false))
        {
            co_return *validationError;
        }
        if (connectionId.empty() || ownerAccountId.empty() || ownerRemoteAccountId.empty())
        {
            co_return OperationError{
                .message = QStringLiteral("Connection, local account, and remote account ids "
                                          "are required for session "
                                          "discovery."),
            };
        }

        javelin::jmap::api::SessionClient sessionClient{*m_impl->resourceTransport};
        const javelin::jmap::auth::SessionRequestContext requestContext{
            .credentials = buildAccountCredentials(settings, ownerAccountId),
            .requiredCapabilities =
                {
                    .mail = true,
                    .submission = false,
                },
        };
        const auto discovered = co_await sessionClient.discover(requestContext);
        if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&discovered))
        {
            co_return operationError(*error);
        }
        if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&discovered))
        {
            co_return operationError(*error);
        }
        if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&discovered))
        {
            co_return operationError(*error);
        }

        const auto& session = std::get<javelin::jmap::api::Session>(discovered);
        javelin::jmap::cache::SessionRepository repository{*m_impl->databaseConnection};
        const auto replaceResult =
            repository.replaceForConnection(connectionId, ownerRemoteAccountId, session);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&replaceResult))
        {
            co_return javelin::jmap::operationError(*error);
        }
        const auto& stored = std::get<javelin::jmap::cache::StoredSessionAccounts>(replaceResult);
        if (stored.ownerAccountId != ownerAccountId)
        {
            co_return OperationError{
                .message = QStringLiteral("The refreshed JMAP account resolved to a different "
                                          "local account identity."),
            };
        }

        const bool websocketAdvertised = session.capabilities.websocket.has_value();
        co_return SessionRefreshSummary{
            .ownerAccountId = stored.ownerAccountId,
            .resolvedSessionUrl = sessionClient.resolvedSessionUrl(),
            .websocketAdvertised = websocketAdvertised,
            .websocketPushSupported =
                websocketAdvertised && session.capabilities.websocket->supportsPush,
        };
    }

    QCoro::Task<LiveRefreshResult>
    AccountBootstrapClient::bootstrap(LiveConnectionSettings settings, std::string connectionId,
                                      std::function<void(const QString&)> progressCallback,
                                      std::vector<std::string> configuredMailboxIds)
    {
        const auto reportProgress = [&progressCallback](const QString& message)
        {
            if (progressCallback)
            {
                progressCallback(message);
            }
        };

        qInfo() << "Account bootstrap start";
        reportProgress(QStringLiteral("Discovering JMAP session..."));
        if (m_impl->databaseConnection == nullptr || m_impl->resourceTransport == nullptr ||
            m_impl->methodTransport == nullptr)
        {
            co_return OperationError{
                .message =
                    QStringLiteral("Live refresh is unavailable in this process configuration."),
            };
        }

        if (const auto validationError = validateLoginSettings(settings, false))
        {
            co_return *validationError;
        }
        if (connectionId.empty())
        {
            co_return OperationError{
                .message = QStringLiteral("A configured connection id is required for bootstrap."),
            };
        }

        javelin::jmap::api::SessionClient sessionClient{*m_impl->resourceTransport};

        const javelin::jmap::auth::SessionRequestContext sessionRequestContext{
            .credentials = buildAccountCredentials(settings, connectionId),
            .requiredCapabilities =
                {
                    .mail = true,
                    .submission = false,
                },
        };

        const auto discovered = co_await sessionClient.discover(sessionRequestContext);
        if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&discovered))
        {
            co_return operationError(*error);
        }
        if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&discovered))
        {
            co_return operationError(*error);
        }
        if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&discovered))
        {
            co_return operationError(*error);
        }

        const auto& session = std::get<javelin::jmap::api::Session>(discovered);
        reportProgress(QStringLiteral("Session discovered. Saving account state..."));
        if (!session.primaryAccounts.mailAccountId.has_value())
        {
            co_return OperationError{
                .message =
                    QStringLiteral("The server session does not expose a primary mail account."),
            };
        }

        const auto& remoteAccountId = *session.primaryAccounts.mailAccountId;
        javelin::jmap::cache::SessionRepository sessionRepository{*m_impl->databaseConnection};
        qInfo() << "Account bootstrap saving session and accounts"
                << "accountCount=" << static_cast<qulonglong>(session.accounts.size());
        const auto replaceResult =
            sessionRepository.replaceForConnection(connectionId, remoteAccountId, session);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&replaceResult))
        {
            co_return javelin::jmap::operationError(*error);
        }
        const auto& storedAccounts =
            std::get<javelin::jmap::cache::StoredSessionAccounts>(replaceResult);
        const auto localAccount = storedAccounts.accountIdsByRemoteId.find(remoteAccountId);
        if (localAccount == storedAccounts.accountIdsByRemoteId.end())
        {
            co_return OperationError{
                .message = QStringLiteral("The primary mail account was not stored locally."),
            };
        }
        const std::string accountId = localAccount->second;
        reportProgress(QStringLiteral("Cached session. Fetching mailboxes..."));
        const auto apiRequestContext = buildApiRequestContext(settings, connectionId, session);
        qInfo() << "Account bootstrap mailbox request context ready";

        javelin::jmap::cache::SyncStateRepository syncStateRepository{*m_impl->databaseConnection};
        javelin::jmap::cache::EmailRepository emailRepository{*m_impl->databaseConnection};
        const javelin::jmap::cache::SyncStateKey emailStateKey{
            .accountId = accountId,
            .objectType = "Email",
            .queryKey = {},
        };
        const auto existingEmailStateResult = syncStateRepository.find(emailStateKey);
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&existingEmailStateResult))
        {
            co_return javelin::jmap::operationError(*error);
        }
        const auto& existingEmailState =
            std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(
                existingEmailStateResult);

        bool localEmailWorkingSetEmpty = true;
        if (!existingEmailState.has_value())
        {
            const auto hasCachedEmails = emailRepository.hasAny(accountId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&hasCachedEmails))
                co_return javelin::jmap::operationError(*error);
            localEmailWorkingSetEmpty = !std::get<bool>(hasCachedEmails);
        }
        const bool establishInitialEmailBaseline =
            !existingEmailState.has_value() && localEmailWorkingSetEmpty;
        if (!existingEmailState.has_value() && !localEmailWorkingSetEmpty)
        {
            qWarning() << "Account bootstrap found cached Email rows without an Email state; "
                          "leaving the cursor unset for account rebaseline"
                       << QString::fromStdString(accountId);
        }

        javelin::jmap::api::MethodCaller methodCaller{*m_impl->methodTransport};
        const auto mailboxRequest = javelin::jmap::api::mailboxGet({.accountId = remoteAccountId,
                                                                    .ids = std::nullopt,
                                                                    .idsReference = std::nullopt,
                                                                    .properties = std::nullopt});
        if (!mailboxRequest.has_value())
        {
            co_return OperationError{
                .message = QStringLiteral("Failed to encode the Mailbox/get request."),
            };
        }
        qInfo() << "Account bootstrap Mailbox/get request encoded";

        javelin::jmap::api::RequestBuilder mailboxRequestBuilder;
        mailboxRequestBuilder.useCore().useMail();
        const auto mailboxHandle = mailboxRequestBuilder.call(*mailboxRequest, "mailboxes");

        std::optional<javelin::jmap::api::CallHandle<javelin::jmap::api::EmailGetResponse>>
            initialEmailStateHandle;
        if (establishInitialEmailBaseline)
        {
            const auto initialEmailStateRequest = javelin::jmap::api::emailGet({
                .accountId = remoteAccountId,
                .ids = std::vector<std::string>{},
                .idsReference = std::nullopt,
                .properties = std::vector<std::string>{"id"},
            });
            if (!initialEmailStateRequest.has_value())
            {
                co_return OperationError{
                    .message = QStringLiteral("Failed to encode the initial Email state request."),
                };
            }
            initialEmailStateHandle =
                mailboxRequestBuilder.call(*initialEmailStateRequest, "initial-email-state");
        }

        const auto mailboxEnvelopeResult =
            co_await methodCaller.call(apiRequestContext, mailboxRequestBuilder);
        if (const auto* error =
                std::get_if<javelin::jmap::api::TransportError>(&mailboxEnvelopeResult))
        {
            co_return operationError(*error);
        }
        if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&mailboxEnvelopeResult))
        {
            co_return operationError(*error);
        }
        if (const auto* error =
                std::get_if<javelin::jmap::api::ProtocolError>(&mailboxEnvelopeResult))
        {
            co_return operationError(*error);
        }

        const auto& mailboxEnvelope =
            std::get<javelin::jmap::api::ResponseEnvelope>(mailboxEnvelopeResult);
        const javelin::jmap::api::ResponseReader mailboxReader{mailboxEnvelope};
        const auto mailboxResult = mailboxReader.require(mailboxHandle);
        if (const auto* error =
                std::get_if<javelin::jmap::api::ResponseReaderError>(&mailboxResult))
        {
            co_return operationError(*error);
        }
        const auto& parsedMailboxes =
            std::get<javelin::jmap::api::MailboxGetResponse>(mailboxResult);
        if (const auto accountError = validateResponseAccountId(
                remoteAccountId, parsedMailboxes.accountId, u"Mailbox/get"))
            co_return *accountError;

        std::optional<std::string> initialEmailState;
        if (initialEmailStateHandle.has_value())
        {
            const auto initialEmailStateResult = mailboxReader.require(*initialEmailStateHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&initialEmailStateResult))
            {
                co_return operationError(*error);
            }
            const auto& parsedInitialEmailState =
                std::get<javelin::jmap::api::EmailGetResponse>(initialEmailStateResult);
            if (const auto accountError = validateResponseAccountId(
                    remoteAccountId, parsedInitialEmailState.accountId, u"Email/get"))
                co_return *accountError;
            if (parsedInitialEmailState.state.empty())
            {
                co_return OperationError{
                    .message = QStringLiteral("The server returned an empty initial Email state."),
                };
            }
            initialEmailState = parsedInitialEmailState.state;
        }

        reportProgress(QStringLiteral("Fetched %1 mailboxes. Updating cache...")
                           .arg(parsedMailboxes.list.size()));

        if (initialEmailState.has_value())
        {
            auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
                *m_impl->databaseConnection, QStringLiteral("Establish initial Email baseline"));
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
                co_return javelin::jmap::operationError(*error);
            auto transaction =
                std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));

            const auto hasCachedEmails = emailRepository.hasAny(transaction, accountId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&hasCachedEmails))
                co_return javelin::jmap::operationError(*error);

            if (!std::get<bool>(hasCachedEmails))
            {
                const auto installed = syncStateRepository.replaceIfCurrent(
                    transaction, emailStateKey, std::nullopt, *initialEmailState);
                if (const auto* error =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&installed))
                    co_return javelin::jmap::operationError(*error);
            }
            if (const auto error = transaction.commit())
                co_return javelin::jmap::operationError(*error);
        }

        javelin::jmap::cache::MailboxRepository mailboxRepository{*m_impl->databaseConnection};
        if (const auto error = mailboxRepository.replaceAll(accountId, parsedMailboxes.list))
        {
            co_return javelin::jmap::operationError(*error);
        }

        if (const auto error = syncStateRepository.upsert(
                {.accountId = accountId, .objectType = "Mailbox", .queryKey = {}},
                parsedMailboxes.state))
        {
            co_return javelin::jmap::operationError(*error);
        }

        auto mailboxIds = std::move(configuredMailboxIds);
        std::erase_if(mailboxIds,
                      [&parsedMailboxes](const auto& mailboxId)
                      {
                          return std::ranges::none_of(parsedMailboxes.list,
                                                      [&mailboxId](const auto& mailbox)
                                                      { return mailbox.id == mailboxId; });
                      });
        std::size_t emailCount = 0;

        if (!mailboxIds.empty())
        {
            javelin::jmap::sync::MailboxRefreshExecutor mailboxRefreshExecutor{
                *m_impl->databaseConnection, methodCaller, apiRequestContext};
            for (const auto& mailboxId : mailboxIds)
            {
                reportProgress(QStringLiteral("Refreshing selected mailbox..."));
                const auto refreshResult = co_await mailboxRefreshExecutor.refreshCollapsedMailbox(
                    accountId, mailboxId, reportProgress, true, remoteAccountId);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&refreshResult))
                {
                    co_return *error;
                }

                emailCount += std::get<javelin::jmap::sync::MailboxRefreshSummary>(refreshResult)
                                  .representativeCount;
            }
            reportProgress(QStringLiteral("Cached %1 threaded conversations.").arg(emailCount));
        }

        qInfo() << "Account bootstrap success"
                << "mailboxCount=" << static_cast<qulonglong>(parsedMailboxes.list.size())
                << "messageCount=" << static_cast<qulonglong>(emailCount);

        co_return LiveRefreshSummary{
            .accountId = accountId,
            .selectedMailboxId = mailboxIds.size() == 1
                                     ? std::optional<std::string>{mailboxIds.front()}
                                     : std::nullopt,
            .mailboxCount = parsedMailboxes.list.size(),
            .emailCount = emailCount,
            .resolvedSessionUrl = sessionClient.resolvedSessionUrl(),
        };
    }

    QCoro::Task<MessageContentRefreshResult>
    MessageContentClient::refresh(LiveConnectionSettings settings, std::string accountId,
                                  std::string emailId,
                                  std::function<void(const QString&)> progressCallback)
    {
        const auto reportProgress = [&progressCallback](const QString& message)
        {
            if (progressCallback)
            {
                progressCallback(message);
            }
        };

        qCDebug(logMessageContent).noquote()
            << "Message content refresh start" << QString::fromStdString(accountId)
            << QString::fromStdString(emailId);
        reportProgress(QStringLiteral("Checking for saved message content..."));
        if (m_impl->databaseConnection == nullptr || m_impl->resourceTransport == nullptr)
        {
            co_return OperationError{
                .message =
                    QStringLiteral("Live refresh is unavailable in this process configuration."),
            };
        }

        if (const auto validationError = validateLoginSettings(settings, false))
        {
            co_return *validationError;
        }

        javelin::jmap::cache::RawMessageSourceRepository sourceRepository{
            *m_impl->databaseConnection};
        const auto emailResult =
            findEmailForDownload(*m_impl->databaseConnection, accountId, emailId);
        if (const auto* error = std::get_if<OperationError>(&emailResult))
        {
            co_return operationError(*error);
        }
        const auto& email = std::get<javelin::jmap::domain::Email>(emailResult);

        const auto cachedBlobId = sourceRepository.findBlobId(accountId, emailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&cachedBlobId))
        {
            co_return operationError(*error);
        }
        const auto& sourceBlobId = std::get<std::optional<std::string>>(cachedBlobId);
        if (sourceBlobId.has_value() && *sourceBlobId == email.blobId)
        {
            qCDebug(logMessageContent).noquote()
                << "Message source using cached data" << QString::fromStdString(emailId);
            reportProgress(QStringLiteral("Opened message from saved content."));
            co_return MessageContentRefreshSummary{
                .accountId = std::move(accountId),
                .emailId = std::move(emailId),
                .partCount = 1,
                .bodyValueCount = 0,
                .usedCachedContent = true,
            };
        }

        const auto sessionResult =
            prepareDownloadContext(*m_impl->databaseConnection, settings, accountId);
        if (const auto* error = std::get_if<OperationError>(&sessionResult))
        {
            co_return *error;
        }
        const auto& context = std::get<DownloadContext>(sessionResult);

        reportProgress(QStringLiteral("Downloading message source from the server..."));
        const javelin::jmap::cache::EmailPart sourcePart{
            .emailId = email.id,
            .partId = email.id,
            .parentPartId = std::nullopt,
            .blobId = email.blobId,
            .kind = "message",
            .mediaType = "message/rfc822",
            .name = std::optional<std::string>{email.id + ".eml"},
            .charset = std::nullopt,
            .disposition = std::nullopt,
            .cid = std::nullopt,
            .size = email.size,
            .isInlineRenderable = false,
            .isBodySection = false,
        };
        const auto incomingResult =
            javelin::jmap::cache::MailVault::forDatabase(*m_impl->databaseConnection)
                .prepareIncoming();
        if (const auto* error = std::get_if<javelin::jmap::cache::MailVaultError>(&incomingResult))
            co_return OperationError{.message = error->message};
        const QString incomingPath = std::get<QString>(incomingResult);
        const auto removeIncoming = qScopeGuard([&incomingPath] { QFile::remove(incomingPath); });
        static_cast<void>(removeIncoming);

        const auto downloadResult = co_await downloadBlobToFile(
            *m_impl->resourceTransport, context.session.downloadUrl, context.remoteAccountId,
            context.credentials.accountId, sourcePart, context.accessToken, incomingPath,
            QStringLiteral("Message source download"));
        if (const auto* error = std::get_if<BlobDownloadError>(&downloadResult))
        {
            if (error->httpStatus == std::optional<int>{404})
            {
                co_return MessageContentUnavailable{
                    .accountId = std::move(accountId),
                    .emailId = std::move(emailId),
                    .message = QStringLiteral(
                        "This message is no longer available on the server (HTTP 404)."),
                };
            }
            co_return error->error;
        }

        const auto payloadSize = std::get<std::uint64_t>(downloadResult);
        auto storeFuture = QtConcurrent::run(storeDownloadedMessageSource,
                                             m_impl->databaseConnection->database().databaseName(),
                                             accountId, email.id, email.blobId, incomingPath);
        const auto storeResult = co_await qCoro(storeFuture).takeResult();
        if (!storeResult.error.isEmpty())
            co_return OperationError{.message = storeResult.error};
        if (!storeResult.stored)
        {
            co_return MessageContentUnavailable{
                .accountId = std::move(accountId),
                .emailId = std::move(emailId),
                .message = QStringLiteral(
                    "The message changed or was removed while its content was downloading."),
            };
        }

        qCDebug(logMessageContent).noquote()
            << "Message source refresh success" << QString::fromStdString(emailId)
            << static_cast<qulonglong>(payloadSize);
        reportProgress(QStringLiteral("Message ready."));

        co_return MessageContentRefreshSummary{
            .accountId = std::move(accountId),
            .emailId = std::move(emailId),
            .partCount = 1,
            .bodyValueCount = 0,
            .usedCachedContent = false,
        };
    }

    QCoro::Task<CollapsedQueryPageResult> MailQueryClient::queryCollapsedPage(
        LiveConnectionSettings settings, std::string accountId,
        javelin::jmap::api::EmailQueryFilter filter, const std::size_t offset,
        const std::size_t limit, javelin::jmap::query::EmailListSort sort,
        std::optional<std::string> anchor, const std::int64_t anchorOffset,
        std::function<void(const QString&)> progressCallback)
    {
        if (m_impl->databaseConnection == nullptr || m_impl->methodTransport == nullptr)
        {
            co_return OperationError{
                .message = QStringLiteral("Mail queries are unavailable."),
            };
        }
        co_return co_await performCollapsedQueryPage(
            *m_impl->databaseConnection, *m_impl->methodTransport, std::move(settings),
            std::move(accountId), std::move(filter), offset, limit, std::move(sort),
            std::move(anchor), anchorOffset, std::move(progressCallback));
    }

    QCoro::Task<EmailIdQueryPageResult>
    MailQueryClient::queryEmailIdsByKeyword(LiveConnectionSettings settings, std::string accountId,
                                            std::string keyword, const std::size_t limit)
    {
        if (m_impl->databaseConnection == nullptr || m_impl->methodTransport == nullptr)
        {
            co_return OperationError{
                .message = QStringLiteral("Keyword queries are unavailable."),
            };
        }
        if (const auto validationError = validateLoginSettings(settings, true))
            co_return *validationError;
        const auto identityResult =
            resolveCachedAccountIdentity(*m_impl->databaseConnection, accountId);
        if (const auto* error = std::get_if<OperationError>(&identityResult))
            co_return *error;
        const auto& identity = std::get<CachedAccountIdentity>(identityResult);
        const auto sessionResult =
            loadCachedSession(*m_impl->databaseConnection, identity.localAccountId);
        if (const auto* error = std::get_if<OperationError>(&sessionResult))
            co_return *error;

        javelin::jmap::api::MethodCaller caller{*m_impl->methodTransport};
        javelin::jmap::api::RequestBuilder builder;
        builder.useCore().useMail();
        const auto queryRequest = javelin::jmap::api::emailQuery({
            .accountId = identity.remoteAccountId,
            .filter = javelin::jmap::api::EmailQueryFilter{.hasKeyword = std::move(keyword)},
            .sort = {},
            .position = std::uint64_t{0},
            .anchor = std::nullopt,
            .anchorOffset = 0,
            .limit = static_cast<std::uint64_t>(limit),
            .collapseThreads = false,
            .calculateTotal = true,
        });
        if (!queryRequest.has_value())
        {
            co_return OperationError{
                .message = QStringLiteral("Failed to encode the keyword Email/query request."),
            };
        }
        const auto queryHandle = builder.call(*queryRequest, "keyword-query");
        const auto envelopeResult = co_await caller.call(
            buildApiRequestContext(settings, identity.localAccountId,
                                   std::get<javelin::jmap::api::Session>(sessionResult)),
            builder);
        if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&envelopeResult))
            co_return operationError(*error);
        if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&envelopeResult))
            co_return operationError(*error);
        if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&envelopeResult))
            co_return operationError(*error);

        const javelin::jmap::api::ResponseReader reader{
            std::get<javelin::jmap::api::ResponseEnvelope>(envelopeResult)};
        const auto queryResult = reader.require(queryHandle);
        if (const auto* error = std::get_if<javelin::jmap::api::ResponseReaderError>(&queryResult))
            co_return operationError(*error);
        const auto& page = std::get<javelin::jmap::api::EmailQueryResponse>(queryResult);
        if (const auto accountError =
                validateResponseAccountId(identity.remoteAccountId, page.accountId, u"Email/query"))
            co_return *accountError;
        co_return EmailIdQueryPage{
            .accountId = std::move(accountId),
            .queryState = page.queryState,
            .total = page.total.has_value()
                         ? std::optional<std::size_t>{static_cast<std::size_t>(*page.total)}
                         : std::nullopt,
            .emailIds = page.ids,
        };
    }

    QCoro::Task<FullMailboxPageResult> MailQueryClient::fetchFullMailboxPage(
        LiveConnectionSettings settings, std::string accountId, std::string mailboxId,
        const std::size_t position, const std::size_t limit, std::optional<std::string> anchor)
    {
        if (m_impl->databaseConnection == nullptr || m_impl->methodTransport == nullptr)
        {
            co_return OperationError{
                .message = QStringLiteral("Full mailbox synchronization is unavailable."),
            };
        }
        if (const auto validationError = validateLoginSettings(settings, true))
            co_return *validationError;
        const auto identityResult =
            resolveCachedAccountIdentity(*m_impl->databaseConnection, accountId);
        if (const auto* error = std::get_if<OperationError>(&identityResult))
            co_return *error;
        const auto& identity = std::get<CachedAccountIdentity>(identityResult);
        const auto sessionResult =
            loadCachedSession(*m_impl->databaseConnection, identity.localAccountId);
        if (const auto* error = std::get_if<OperationError>(&sessionResult))
            co_return *error;
        const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);
        const auto requestLimits = javelin::jmap::api::coreRequestLimits(session);
        if (!requestLimits.has_value())
        {
            co_return OperationError{
                .message = QStringLiteral("The cached JMAP session has invalid request limits."),
            };
        }
        const auto boundedLimit = static_cast<std::size_t>(std::min<std::uint64_t>(
            requestLimits->maxObjectsInGet, static_cast<std::uint64_t>(limit)));
        if (boundedLimit == 0)
        {
            co_return OperationError{
                .message = QStringLiteral("Full mailbox page size must be greater than zero."),
            };
        }

        javelin::jmap::api::MethodCaller caller{*m_impl->methodTransport};
        javelin::jmap::api::RequestBuilder builder;
        builder.useCore().useMail();
        const auto queryRequest = javelin::jmap::api::emailQuery({
            .accountId = identity.remoteAccountId,
            .filter = javelin::jmap::api::EmailQueryFilter{.inMailbox = mailboxId},
            .sort = {javelin::jmap::api::EmailQuerySort{.property = "receivedAt",
                                                        .isAscending = false}},
            .position = anchor.has_value()
                            ? std::nullopt
                            : std::optional<std::uint64_t>{static_cast<std::uint64_t>(position)},
            .anchor = std::move(anchor),
            .anchorOffset = static_cast<std::int64_t>(position),
            .limit = static_cast<std::uint64_t>(boundedLimit),
            .collapseThreads = false,
            .calculateTotal = true,
        });
        if (!queryRequest.has_value())
        {
            co_return OperationError{
                .message = QStringLiteral("Failed to encode the full mailbox query."),
            };
        }
        const auto queryHandle = builder.call(*queryRequest, "full-mailbox-query");
        const auto getRequest = javelin::jmap::api::emailGet(javelin::jmap::api::getRequestFrom(
            identity.remoteAccountId, queryHandle, "/ids",
            std::vector<std::string>{"id", "blobId", "threadId", "mailboxIds", "keywords", "size",
                                     "receivedAt", "sentAt", "messageId", "inReplyTo", "references",
                                     "hasAttachment", "subject", "from", "to", "cc", "bcc",
                                     "replyTo"}));
        if (!getRequest.has_value())
        {
            co_return OperationError{
                .message = QStringLiteral("Failed to encode full mailbox metadata retrieval."),
            };
        }
        const auto getHandle = builder.call(*getRequest, "full-mailbox-get");
        const auto envelopeResult = co_await caller.call(
            buildApiRequestContext(settings, identity.localAccountId, session), builder);
        if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&envelopeResult))
            co_return operationError(*error);
        if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&envelopeResult))
            co_return operationError(*error);
        if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&envelopeResult))
            co_return operationError(*error);

        const javelin::jmap::api::ResponseReader reader{
            std::get<javelin::jmap::api::ResponseEnvelope>(envelopeResult)};
        const auto queryResult = reader.require(queryHandle);
        if (const auto* error = std::get_if<javelin::jmap::api::ResponseReaderError>(&queryResult))
            co_return operationError(*error);
        const auto getResult = reader.require(getHandle);
        if (const auto* error = std::get_if<javelin::jmap::api::ResponseReaderError>(&getResult))
            co_return operationError(*error);

        const auto& page = std::get<javelin::jmap::api::EmailQueryResponse>(queryResult);
        const auto& emails = std::get<javelin::jmap::api::EmailGetResponse>(getResult);
        if (const auto accountError =
                validateResponseAccountId(identity.remoteAccountId, page.accountId, u"Email/query"))
            co_return *accountError;
        if (const auto accountError =
                validateResponseAccountId(identity.remoteAccountId, emails.accountId, u"Email/get"))
            co_return *accountError;
        if (emails.list.size() != page.ids.size())
        {
            co_return OperationError{
                .message = QStringLiteral("Email/get omitted messages from the full mailbox page."),
            };
        }
        co_return FullMailboxPage{
            .accountId = std::move(accountId),
            .mailboxId = std::move(mailboxId),
            .queryState = page.queryState,
            .position = static_cast<std::size_t>(page.position),
            .total = page.total.has_value()
                         ? std::optional<std::size_t>{static_cast<std::size_t>(*page.total)}
                         : std::nullopt,
            .emailIds = page.ids,
            .emails = emails.list,
            .emailState = emails.state,
        };
    }

    QCoro::Task<AttachmentDownloadResult>
    MessageContentClient::loadAttachment(std::string accountId, std::string emailId,
                                         std::string partId)
    {
        if (m_impl->databaseConnection == nullptr)
        {
            co_return OperationError{
                .message = QStringLiteral(
                    "Attachment download is unavailable in this process configuration."),
            };
        }

        const auto emailResult =
            findEmailForDownload(*m_impl->databaseConnection, accountId, emailId);
        if (const auto* error = std::get_if<OperationError>(&emailResult))
        {
            co_return *error;
        }
        const auto& email = std::get<javelin::jmap::domain::Email>(emailResult);

        javelin::jmap::cache::RawMessageSourceRepository sourceRepository{
            *m_impl->databaseConnection};
        const auto sourceResult = sourceRepository.find(accountId, emailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&sourceResult))
        {
            co_return javelin::jmap::operationError(*error);
        }

        const auto& source =
            std::get<std::optional<javelin::jmap::cache::RawMessageSource>>(sourceResult);
        if (!source.has_value() || source->blobId != email.blobId)
        {
            co_return OperationError{
                .message = QStringLiteral(
                    "The selected attachment is not cached locally. Open the message first."),
            };
        }

        const auto parsedPart =
            javelin::jmap::cache::findMessageSourcePart(emailId, source->payload, partId);
        if (!parsedPart.has_value())
        {
            co_return OperationError{
                .message =
                    QStringLiteral("The selected attachment is not present in the cached message."),
            };
        }

        const auto& part = parsedPart->part;
        const bool isAttachment = part.kind == "attachment" || part.name.has_value() ||
                                  part.disposition.has_value() || part.cid.has_value();
        if (!isAttachment)
        {
            co_return OperationError{
                .message = QStringLiteral("The selected message part is not downloadable."),
            };
        }

        co_return AttachmentDownload{
            .accountId = std::move(accountId),
            .emailId = std::move(emailId),
            .partId = std::move(partId),
            .name = part.name,
            .mediaType = part.mediaType,
            .payload = parsedPart->payload,
            .usedCachedInlinePayload = true,
        };
    }

    QCoro::Task<MessageSourceDownloadResult>
    MessageContentClient::loadCachedSource(std::string accountId, std::string emailId)
    {
        if (m_impl->databaseConnection == nullptr)
        {
            co_return OperationError{
                .message = QStringLiteral(
                    "Cached message source is unavailable in this process configuration."),
            };
        }

        const auto emailResult =
            findEmailForDownload(*m_impl->databaseConnection, accountId, emailId);
        if (const auto* error = std::get_if<OperationError>(&emailResult))
        {
            co_return *error;
        }

        const auto& email = std::get<javelin::jmap::domain::Email>(emailResult);
        javelin::jmap::cache::RawMessageSourceRepository sourceRepository{
            *m_impl->databaseConnection};
        const auto sourceResult = sourceRepository.find(accountId, emailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&sourceResult))
        {
            co_return operationError(*error);
        }

        const auto& source =
            std::get<std::optional<javelin::jmap::cache::RawMessageSource>>(sourceResult);
        if (!source.has_value() || source->blobId != email.blobId)
        {
            co_return OperationError{
                .message = QStringLiteral(
                    "The message source is not cached locally. Open the message first."),
            };
        }

        co_return MessageSourceDownload{
            .accountId = std::move(accountId),
            .emailId = std::move(emailId),
            .blobId = email.blobId,
            .subject = email.subject,
            .payload = source->payload,
        };
    }

    QueuedEmailMutationResult EmailMutationEngine::queue(std::string accountId,
                                                         EmailMailboxMutation mutation)
    {
        if (m_impl->databaseConnection == nullptr)
        {
            return OperationError{
                .message = QStringLiteral("Queued mutations are unavailable in this process."),
            };
        }

        return sync::queueEmailMutation(*m_impl->databaseConnection, std::move(accountId),
                                        std::move(mutation));
    }

    QueuedEmailMutationsResult
    EmailMutationEngine::queueBatch(std::string accountId,
                                    std::vector<EmailMailboxMutation> mutations)
    {
        if (m_impl->databaseConnection == nullptr)
        {
            return OperationError{
                .message = QStringLiteral("Queued mutations are unavailable in this process."),
            };
        }
        return sync::queueEmailMutations(*m_impl->databaseConnection, std::move(accountId),
                                         std::move(mutations));
    }

    QCoro::Task<SubmittedEmailMutationsResult>
    EmailMutationEngine::submitPending(LiveConnectionSettings settings, std::string accountId,
                                       std::optional<std::string> operationGroupId,
                                       const std::size_t limit,
                                       std::optional<std::string> ifInStateOverride)
    {
        if (m_impl->databaseConnection == nullptr || m_impl->methodTransport == nullptr)
        {
            co_return OperationError{
                .message = QStringLiteral("Required mail capability dependencies are unavailable."),
            };
        }

        if (const auto validationError = validateLoginSettings(settings, true))
        {
            co_return *validationError;
        }

        const auto identityResult =
            resolveCachedAccountIdentity(*m_impl->databaseConnection, accountId);
        if (const auto* error = std::get_if<OperationError>(&identityResult))
            co_return *error;
        const auto& identity = std::get<CachedAccountIdentity>(identityResult);

        const auto sessionResult =
            loadCachedSession(*m_impl->databaseConnection, identity.localAccountId);
        if (const auto* error = std::get_if<OperationError>(&sessionResult))
        {
            co_return *error;
        }
        const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);
        const auto requestLimits = javelin::jmap::api::coreRequestLimits(session);
        if (!requestLimits.has_value())
        {
            co_return OperationError{
                .message = QStringLiteral("The cached JMAP session has invalid request limits."),
            };
        }
        const auto batchLimit = static_cast<std::size_t>(std::min<std::uint64_t>(
            requestLimits->maxObjectsInSet, static_cast<std::uint64_t>(limit)));
        if (batchLimit == 0)
        {
            co_return OperationError{
                .message = QStringLiteral("Email/set batch size must be greater than zero."),
            };
        }

        javelin::jmap::sync::EmailMutationJournal emailMutationJournal{*m_impl->databaseConnection};
        auto pendingResult =
            operationGroupId.has_value()
                ? emailMutationJournal.listPendingForOperationGroup(accountId, *operationGroupId,
                                                                    batchLimit)
                : emailMutationJournal.listByStatus(
                      accountId, javelin::jmap::sync::MutationStatus::Pending, batchLimit);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&pendingResult))
        {
            co_return javelin::jmap::operationError(*error);
        }

        auto pendingActions = std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(
            std::move(pendingResult));
        std::erase_if(pendingActions, [](const auto& action)
                      { return action.status != javelin::jmap::sync::MutationStatus::Pending; });
        if (pendingActions.empty())
        {
            co_return SubmittedEmailMutations{
                .accountId = std::move(accountId),
                .attemptedEmailCount = 0,
                .updatedEmailCount = 0,
                .failedEmailCount = 0,
                .statePreconditionUsed = false,
                .items = {},
                .receipt = {},
            };
        }

        const bool hasIfInStateOverride = ifInStateOverride.has_value();
        auto ifInState = std::move(ifInStateOverride);
        for (const auto& action : pendingActions)
        {
            if (hasIfInStateOverride)
                break;
            if (!action.baseState.has_value())
                continue;
            if (ifInState.has_value() && *ifInState != *action.baseState)
            {
                co_return OperationError{
                    .message = QStringLiteral(
                        "A mutation group contains inconsistent Email state preconditions."),
                };
            }
            ifInState = action.baseState;
        }

        std::vector<std::string> emailIds;
        std::unordered_set<std::string> seenEmailIds;
        for (const auto& action : pendingActions)
        {
            if (seenEmailIds.insert(action.patch.emailId).second)
            {
                emailIds.push_back(action.patch.emailId);
            }
        }

        javelin::jmap::cache::EmailRepository emailRepository{*m_impl->databaseConnection};
        std::unordered_map<std::string, javelin::jmap::domain::Email> mergedEmails;
        std::unordered_map<std::string, std::vector<std::string>> mutationIdsByEmailId;
        std::unordered_map<std::string, std::vector<javelin::jmap::sync::EmailMutationRecord>>
            mutationsByEmailId;
        std::unordered_set<std::string> uncachedEmailIds;
        for (const auto& emailId : emailIds)
        {
            std::vector<javelin::jmap::sync::EmailMutationRecord> selectedActions;
            for (const auto& action : pendingActions)
            {
                if (action.patch.emailId == emailId)
                    selectedActions.push_back(action);
            }
            if (selectedActions.empty())
                continue;

            const auto emailResult = emailRepository.find(accountId, emailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&emailResult))
            {
                co_return javelin::jmap::operationError(*error);
            }

            javelin::jmap::domain::Email baseEmail;
            const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(emailResult);
            if (email.has_value())
            {
                baseEmail = *email;
            }
            else
            {
                const auto& first = selectedActions.front();
                if (!first.baseMailboxIds.has_value() || !first.baseKeywords.has_value())
                {
                    co_return OperationError{
                        .message = QStringLiteral(
                            "An uncached queued email mutation has no authoritative base."),
                    };
                }
                baseEmail.id = emailId;
                baseEmail.mailboxIds = *first.baseMailboxIds;
                baseEmail.keywords = *first.baseKeywords;
                uncachedEmailIds.insert(emailId);
            }

            const auto allEmailActionsResult =
                emailMutationJournal.listForEmail(accountId, emailId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&allEmailActionsResult))
            {
                co_return javelin::jmap::operationError(*error);
            }

            const auto allEmailActions = activeEmailMutations(
                std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(
                    allEmailActionsResult));
            if (allEmailActions.empty())
                continue;
            mutationsByEmailId.emplace(emailId, selectedActions);
            mergedEmails.emplace(emailId, javelin::jmap::sync::projectEmailMutations(
                                              std::move(baseEmail), allEmailActions));

            auto& pendingIds = mutationIdsByEmailId[emailId];
            pendingIds.reserve(selectedActions.size());
            for (const auto& action : selectedActions)
            {
                pendingIds.push_back(action.mutationId);
                if (const auto error = emailMutationJournal.transition(
                        action.mutationId, javelin::jmap::sync::MutationStatus::InFlight))
                {
                    co_return javelin::jmap::operationError(*error);
                }
            }
        }

        std::unordered_map<std::string, javelin::jmap::api::EmailSetUpdate> updates;
        std::vector<std::string> destroys;
        updates.reserve(mergedEmails.size());
        destroys.reserve(mergedEmails.size());
        for (const auto& [emailId, email] : mergedEmails)
        {
            static_cast<void>(email);
            const auto actionsIt = mutationsByEmailId.find(emailId);
            if (actionsIt == mutationsByEmailId.end())
                continue;
            const auto& actions = actionsIt->second;
            const bool destroy = std::ranges::any_of(actions, [](const auto& action)
                                                     { return action.patch.destroy; });
            if (destroy)
            {
                destroys.push_back(emailId);
                continue;
            }

            javelin::jmap::api::EmailSetUpdate update;
            for (const auto& action : actions)
            {
                for (const auto& mailboxId : action.patch.addMailboxIds)
                {
                    update.patch.insert_or_assign(
                        javelin::jmap::api::patchPath("mailboxIds", mailboxId), true);
                }
                for (const auto& mailboxId : action.patch.removeMailboxIds)
                {
                    update.patch.insert_or_assign(
                        javelin::jmap::api::patchPath("mailboxIds", mailboxId), nullptr);
                }
                for (const auto& keyword : action.patch.addKeywords)
                {
                    update.patch.insert_or_assign(
                        javelin::jmap::api::patchPath("keywords", keyword), true);
                }
                for (const auto& keyword : action.patch.removeKeywords)
                {
                    update.patch.insert_or_assign(
                        javelin::jmap::api::patchPath("keywords", keyword), nullptr);
                }
            }
            updates.emplace(emailId, std::move(update));
        }

        const bool statePreconditionUsed = ifInState.has_value();
        const auto requestMethod = javelin::jmap::api::emailSet({
            .accountId = identity.remoteAccountId,
            .ifInState = std::move(ifInState),
            .create = {},
            .update = std::move(updates),
            .destroy = std::move(destroys),
        });
        if (!requestMethod.has_value())
        {
            co_return OperationError{
                .message = QStringLiteral("Failed to encode the Email/set request."),
            };
        }

        javelin::jmap::api::MethodCaller methodCaller{*m_impl->methodTransport};
        const auto apiRequestContext =
            buildApiRequestContext(settings, identity.localAccountId, session);
        javelin::jmap::api::RequestBuilder requestBuilder;
        requestBuilder.useCore().useMail();
        const auto setHandle = requestBuilder.call(*requestMethod, "queued-email-set");
        const bool mailboxCountsMayChange = std::ranges::any_of(
            pendingActions,
            [](const auto& action)
            {
                const auto affectsUnreadCount = [](const auto& keywords)
                {
                    return std::ranges::contains(keywords, std::string{"$seen"}) ||
                           std::ranges::contains(keywords, std::string{"$draft"});
                };
                return action.patch.destroy || !action.patch.addMailboxIds.empty() ||
                       !action.patch.removeMailboxIds.empty() ||
                       affectsUnreadCount(action.patch.addKeywords) ||
                       affectsUnreadCount(action.patch.removeKeywords);
            });
        std::optional<javelin::jmap::api::CallHandle<javelin::jmap::api::MailboxGetResponse>>
            mailboxHandle;
        if (mailboxCountsMayChange)
        {
            const auto mailboxRequest = javelin::jmap::api::mailboxGet({
                .accountId = identity.remoteAccountId,
                .ids = std::nullopt,
                .idsReference = std::nullopt,
                .properties = std::nullopt,
            });
            if (!mailboxRequest.has_value())
            {
                co_return OperationError{
                    .message = QStringLiteral("Failed to encode the post-mutation Mailbox/get."),
                };
            }
            mailboxHandle = requestBuilder.call(*mailboxRequest, "queued-email-mailboxes");
        }

        const auto transitionSubmittedMutations =
            [&emailMutationJournal, &mutationIdsByEmailId](
                const javelin::jmap::sync::MutationStatus status) -> std::optional<OperationError>
        {
            for (const auto& [emailId, mutationIds] : mutationIdsByEmailId)
            {
                static_cast<void>(emailId);
                for (const auto& mutationId : mutationIds)
                {
                    if (const auto error = emailMutationJournal.transition(mutationId, status))
                    {
                        return javelin::jmap::operationError(*error);
                    }
                }
            }
            return std::nullopt;
        };
        const auto rejectSubmittedMutations =
            [this, &accountId, &emailMutationJournal, &mergedEmails, &mutationsByEmailId,
             &mutationIdsByEmailId, &uncachedEmailIds]() -> std::optional<OperationError>
        {
            auto transactionResult = javelin::jmap::sync::MutationProjectionTransaction::begin(
                *m_impl->databaseConnection,
                QStringLiteral("Reject Email mutation method failure"));
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            {
                return javelin::jmap::operationError(*error);
            }
            auto transaction = std::get<javelin::jmap::sync::MutationProjectionTransaction>(
                std::move(transactionResult));
            javelin::jmap::cache::EmailRepository emails{*m_impl->databaseConnection};
            for (const auto& [emailId, projectedEmail] : mergedEmails)
            {
                const auto actions = mutationsByEmailId.find(emailId);
                if (actions == mutationsByEmailId.end() || actions->second.empty() ||
                    !actions->second.front().baseMailboxIds.has_value() ||
                    !actions->second.front().baseKeywords.has_value())
                {
                    return OperationError{
                        .code = OperationErrorCode::LocalStorageFailure,
                        .message = QStringLiteral(
                            "A failed Email mutation is missing its rollback snapshot."),
                    };
                }
                auto restored = projectedEmail;
                restored.mailboxIds = *actions->second.front().baseMailboxIds;
                restored.keywords = *actions->second.front().baseKeywords;
                for (const auto& mutationId : mutationIdsByEmailId.at(emailId))
                {
                    if (const auto error = transaction.transition(
                            mutationId, javelin::jmap::sync::MutationStatus::Rejected))
                    {
                        return javelin::jmap::operationError(*error);
                    }
                }
                auto recordsResult =
                    loadEmailMutationRecords(emailMutationJournal, accountId, emailId);
                if (const auto* error = std::get_if<OperationError>(&recordsResult))
                    return *error;
                const auto& records = std::get<EmailMutationRecords>(recordsResult);
                auto reprojected = reprojectActiveEmailMutations(std::move(restored), records);
                if (!uncachedEmailIds.contains(emailId))
                {
                    if (const auto error = emails.upsertMany(transaction.cacheTransaction(),
                                                             accountId, {reprojected}))
                    {
                        return javelin::jmap::operationError(*error);
                    }
                }
                if (const auto error = compactSettledEmailMutations(transaction, records))
                    return *error;
            }
            if (const auto error = transaction.commit())
                return javelin::jmap::operationError(*error);
            return std::nullopt;
        };

        const auto envelopeResult = co_await methodCaller.call(apiRequestContext, requestBuilder);
        if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&envelopeResult))
        {
            if (const auto transitionError =
                    transitionSubmittedMutations(javelin::jmap::sync::MutationStatus::Unknown))
            {
                co_return *transitionError;
            }
            co_return operationError(*error);
        }
        if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&envelopeResult))
        {
            if (const auto transitionError =
                    transitionSubmittedMutations(javelin::jmap::sync::MutationStatus::Pending))
            {
                co_return *transitionError;
            }
            co_return operationError(*error);
        }
        if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&envelopeResult))
        {
            if (const auto transitionError =
                    transitionSubmittedMutations(javelin::jmap::sync::MutationStatus::Unknown))
            {
                co_return *transitionError;
            }
            co_return operationError(*error);
        }

        const auto& envelope = std::get<javelin::jmap::api::ResponseEnvelope>(envelopeResult);
        const javelin::jmap::api::ResponseReader reader{envelope};
        const auto parsedResult = reader.require(setHandle);
        if (const auto* error = std::get_if<javelin::jmap::api::ResponseReaderError>(&parsedResult))
        {
            if (error->code == javelin::jmap::api::ResponseReaderErrorCode::MethodError)
            {
                if (const auto rejectionError = rejectSubmittedMutations())
                    co_return *rejectionError;
            }
            else if (const auto transitionError =
                         transitionSubmittedMutations(javelin::jmap::sync::MutationStatus::Unknown))
            {
                co_return *transitionError;
            }
            co_return operationError(*error);
        }
        const auto& parsed = std::get<javelin::jmap::api::EmailSetResponse>(parsedResult);
        if (parsed.accountId != identity.remoteAccountId)
        {
            if (const auto transitionError =
                    transitionSubmittedMutations(javelin::jmap::sync::MutationStatus::Unknown))
            {
                co_return *transitionError;
            }
            co_return OperationError{
                .code = OperationErrorCode::ProtocolViolation,
                .message = QStringLiteral("The server returned Email/set for another account."),
            };
        }
        std::optional<javelin::jmap::api::MailboxGetResponse> parsedMailboxes;
        if (mailboxHandle.has_value())
        {
            const auto mailboxResult = reader.require(*mailboxHandle);
            if (const auto* mailboxError =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&mailboxResult))
            {
                qWarning().noquote() << "Post-mutation Mailbox/get was incomplete; a later "
                                        "push will reconcile it"
                                     << operationError(*mailboxError).message;
            }
            else
            {
                auto response = std::get<javelin::jmap::api::MailboxGetResponse>(mailboxResult);
                if (const auto accountError = validateResponseAccountId(
                        identity.remoteAccountId, response.accountId, u"Mailbox/get"))
                {
                    qWarning().noquote()
                        << "Post-mutation Mailbox/get was for another account; a later push will "
                           "reconcile it"
                        << accountError->message;
                }
                else
                {
                    parsedMailboxes = std::move(response);
                }
            }
        }

        std::unordered_set<std::string> updatedEmailIds{parsed.updated.begin(),
                                                        parsed.updated.end()};
        std::unordered_set<std::string> destroyedEmailIds{parsed.destroyed.begin(),
                                                          parsed.destroyed.end()};
        std::unordered_set<std::string> failedEmailIds{parsed.notUpdated.begin(),
                                                       parsed.notUpdated.end()};
        failedEmailIds.insert(parsed.notDestroyed.begin(), parsed.notDestroyed.end());
        if (!parsed.notDestroyed.empty())
        {
            qWarning().noquote() << "Email/set permanent deletion rejected for ids"
                                 << QString::fromStdString(parsed.notDestroyed.front()) << "count"
                                 << static_cast<qulonglong>(parsed.notDestroyed.size());
        }

        auto transactionResult = javelin::jmap::sync::MutationProjectionTransaction::begin(
            *m_impl->databaseConnection, QStringLiteral("Apply Email mutation response"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
        {
            co_return javelin::jmap::operationError(*error);
        }
        auto transaction = std::get<javelin::jmap::sync::MutationProjectionTransaction>(
            std::move(transactionResult));

        if (!updatedEmailIds.empty() || !destroyedEmailIds.empty())
        {
            const std::array domains{javelin::jmap::sync::ConsistencyDomain{
                .accountId = accountId,
                .dataType = "Email",
            }};
            if (const auto error = transaction.advance(domains))
            {
                co_return javelin::jmap::operationError(*error);
            }

            javelin::jmap::cache::SyncStateRepository states{*m_impl->databaseConnection};
            const auto stateAdvanced = states.advanceIfCurrent(
                transaction.cacheTransaction(),
                {.accountId = accountId, .objectType = "Email", .queryKey = {}}, parsed.oldState,
                parsed.newState);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&stateAdvanced))
            {
                co_return javelin::jmap::operationError(*error);
            }
        }

        if (parsedMailboxes.has_value())
        {
            const std::array mailboxDomains{javelin::jmap::sync::ConsistencyDomain{
                .accountId = accountId,
                .dataType = "Mailbox",
            }};
            if (const auto error = transaction.advance(mailboxDomains))
                co_return javelin::jmap::operationError(*error);
            javelin::jmap::cache::MailboxRepository mailboxes{*m_impl->databaseConnection};
            if (const auto error = mailboxes.replaceAll(transaction.cacheTransaction(), accountId,
                                                        parsedMailboxes->list))
                co_return javelin::jmap::operationError(*error);
            javelin::jmap::cache::SyncStateRepository states{*m_impl->databaseConnection};
            if (const auto error =
                    states.upsert(transaction.cacheTransaction(),
                                  {.accountId = accountId, .objectType = "Mailbox", .queryKey = {}},
                                  parsedMailboxes->state))
                co_return javelin::jmap::operationError(*error);
        }

        for (const auto& [emailId, email] : mergedEmails)
        {
            const auto idsIt = mutationIdsByEmailId.find(emailId);
            if (idsIt == mutationIdsByEmailId.end())
            {
                continue;
            }

            if (updatedEmailIds.contains(emailId) || destroyedEmailIds.contains(emailId))
            {
                for (const auto& mutationId : idsIt->second)
                {
                    if (const auto error = transaction.transition(
                            mutationId, javelin::jmap::sync::MutationStatus::Accepted,
                            parsed.newState))
                    {
                        co_return javelin::jmap::operationError(*error);
                    }
                }

                auto recordsResult =
                    loadEmailMutationRecords(emailMutationJournal, accountId, emailId);
                if (const auto* error = std::get_if<OperationError>(&recordsResult))
                    co_return *error;
                const auto& records = std::get<EmailMutationRecords>(recordsResult);

                if (destroyedEmailIds.contains(emailId))
                {
                    const std::array destroyed{emailId};
                    if (const auto error = emailRepository.removeMany(
                            transaction.cacheTransaction(), accountId, destroyed))
                    {
                        co_return javelin::jmap::operationError(*error);
                    }
                }
                if (const auto error = compactSettledEmailMutations(transaction, records))
                    co_return *error;
                continue;
            }

            if (failedEmailIds.contains(emailId))
            {
                const auto actionsIt = mutationsByEmailId.find(emailId);
                if (actionsIt == mutationsByEmailId.end() || actionsIt->second.empty() ||
                    !actionsIt->second.front().baseMailboxIds.has_value() ||
                    !actionsIt->second.front().baseKeywords.has_value())
                {
                    co_return OperationError{
                        .code = OperationErrorCode::LocalStorageFailure,
                        .message = QStringLiteral(
                            "A rejected Email mutation is missing its rollback snapshot."),
                    };
                }
                auto restored = email;
                restored.mailboxIds = *actionsIt->second.front().baseMailboxIds;
                restored.keywords = *actionsIt->second.front().baseKeywords;
                for (const auto& mutationId : idsIt->second)
                {
                    if (const auto error = transaction.transition(
                            mutationId, javelin::jmap::sync::MutationStatus::Rejected))
                    {
                        co_return javelin::jmap::operationError(*error);
                    }
                }
                auto recordsResult =
                    loadEmailMutationRecords(emailMutationJournal, accountId, emailId);
                if (const auto* error = std::get_if<OperationError>(&recordsResult))
                    co_return *error;
                const auto& records = std::get<EmailMutationRecords>(recordsResult);
                auto reprojected = reprojectActiveEmailMutations(std::move(restored), records);
                if (!uncachedEmailIds.contains(emailId))
                {
                    if (const auto error = emailRepository.upsertMany(
                            transaction.cacheTransaction(), accountId, {reprojected}))
                    {
                        co_return javelin::jmap::operationError(*error);
                    }
                }
                if (const auto error = compactSettledEmailMutations(transaction, records))
                    co_return *error;
                continue;
            }
            for (const auto& mutationId : idsIt->second)
            {
                if (const auto error = transaction.transition(
                        mutationId, javelin::jmap::sync::MutationStatus::Pending))
                {
                    co_return javelin::jmap::operationError(*error);
                }
            }
        }

        if (statePreconditionUsed && operationGroupId.has_value())
        {
            if (const auto error = transaction.rebasePendingOperationGroup(
                    {.accountId = accountId, .dataType = "Email"}, *operationGroupId,
                    parsed.newState))
            {
                co_return javelin::jmap::operationError(*error);
            }
        }

        if (const auto error = transaction.commit())
        {
            co_return javelin::jmap::operationError(*error);
        }

        std::vector<SubmittedEmailMutations::Item> items;
        items.reserve(mutationIdsByEmailId.size());
        for (auto& [emailId, mutationIds] : mutationIdsByEmailId)
        {
            const bool accepted =
                updatedEmailIds.contains(emailId) || destroyedEmailIds.contains(emailId);
            items.push_back({
                .emailId = emailId,
                .mutationIds = std::move(mutationIds),
                .accepted = accepted,
                .error = failedEmailIds.contains(emailId)
                             ? std::optional<std::string>{"Email/set rejected the object."}
                             : std::nullopt,
            });
        }

        co_return SubmittedEmailMutations{
            .accountId = accountId,
            .attemptedEmailCount = mergedEmails.size(),
            .updatedEmailCount = updatedEmailIds.size() + destroyedEmailIds.size(),
            .failedEmailCount = failedEmailIds.size(),
            .statePreconditionUsed = statePreconditionUsed,
            .items = std::move(items),
            .receipt =
                {
                    .domains =
                        {
                            {
                                .accountId = accountId,
                                .dataType = "Email",
                                .oldState = parsed.oldState,
                                .newState = parsed.newState,
                            },
                        },
                    .acceptedObjectIds =
                        [&]
                    {
                        std::vector<std::string> ids(parsed.updated.begin(), parsed.updated.end());
                        ids.insert(ids.end(), parsed.destroyed.begin(), parsed.destroyed.end());
                        return ids;
                    }(),
                    .rejectedObjectIds =
                        std::vector<std::string>(failedEmailIds.begin(), failedEmailIds.end()),
                    .affectedCacheViews = {"mailbox", "search"},
                    .incompleteMaterialization =
                        mailboxHandle.has_value() && !parsedMailboxes.has_value(),
                },
        };
    }

    QCoro::Task<AuthoritativeEmailsResult>
    EmailMutationEngine::getAuthoritative(LiveConnectionSettings settings, std::string accountId,
                                          std::vector<std::string> emailIds)
    {
        if (m_impl->databaseConnection == nullptr || m_impl->methodTransport == nullptr)
        {
            co_return OperationError{
                .message = QStringLiteral("Required mail capability dependencies are unavailable."),
            };
        }
        if (const auto validationError = validateLoginSettings(settings, true))
            co_return *validationError;

        const auto identityResult =
            resolveCachedAccountIdentity(*m_impl->databaseConnection, accountId);
        if (const auto* error = std::get_if<OperationError>(&identityResult))
            co_return *error;
        const auto& identity = std::get<CachedAccountIdentity>(identityResult);
        const auto sessionResult =
            loadCachedSession(*m_impl->databaseConnection, identity.localAccountId);
        if (const auto* error = std::get_if<OperationError>(&sessionResult))
            co_return *error;
        const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);

        const auto getMethod = javelin::jmap::api::emailGet({
            .accountId = identity.remoteAccountId,
            .ids = std::move(emailIds),
            .idsReference = std::nullopt,
            .properties = std::vector<std::string>{"id", "mailboxIds", "keywords", "subject"},
        });
        if (!getMethod.has_value())
        {
            co_return OperationError{
                .code = OperationErrorCode::InvalidRequest,
                .message = QStringLiteral("Failed to encode authoritative Email/get."),
            };
        }

        javelin::jmap::api::RequestBuilder builder;
        builder.useCore().useMail();
        const auto handle = builder.call(*getMethod, "history-email-get");
        javelin::jmap::api::MethodCaller caller{*m_impl->methodTransport};
        const auto result = co_await caller.call(
            buildApiRequestContext(settings, identity.localAccountId, session), builder);
        if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&result))
            co_return operationError(*error);
        if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&result))
            co_return operationError(*error);
        if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&result))
            co_return operationError(*error);

        const javelin::jmap::api::ResponseReader reader{
            std::get<javelin::jmap::api::ResponseEnvelope>(result)};
        const auto parsed = reader.require(handle);
        if (const auto* error = std::get_if<javelin::jmap::api::ResponseReaderError>(&parsed))
            co_return operationError(*error);
        const auto& response = std::get<javelin::jmap::api::EmailGetResponse>(parsed);
        if (response.accountId != identity.remoteAccountId)
        {
            co_return OperationError{
                .code = OperationErrorCode::ProtocolViolation,
                .message = QStringLiteral("The server returned Email/get for another account."),
            };
        }
        co_return AuthoritativeEmails{
            .accountId = std::move(accountId),
            .state = response.state,
            .emails = response.list,
            .notFound = response.notFound,
        };
    }

    QCoro::Task<MailboxSubscriptionChangeResult>
    MailboxMutationEngine::setSubscribed(LiveConnectionSettings settings, std::string accountId,
                                         std::string mailboxId, const bool subscribed,
                                         std::function<void()> projectionCommitted)
    {
        if (m_impl->databaseConnection == nullptr || m_impl->methodTransport == nullptr)
        {
            co_return OperationError{
                .message = QStringLiteral("Required mail capability dependencies are unavailable."),
            };
        }
        if (const auto validationError = validateLoginSettings(settings, true))
            co_return *validationError;
        const auto identityResult =
            resolveCachedAccountIdentity(*m_impl->databaseConnection, accountId);
        if (const auto* error = std::get_if<OperationError>(&identityResult))
            co_return *error;
        const auto& identity = std::get<CachedAccountIdentity>(identityResult);
        const auto sessionResult =
            loadCachedSession(*m_impl->databaseConnection, identity.localAccountId);
        if (const auto* error = std::get_if<OperationError>(&sessionResult))
            co_return *error;
        const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);
        if (!session.capabilities.mail)
        {
            co_return OperationError{
                .code = OperationErrorCode::UnsupportedCapability,
                .message = QStringLiteral("This server does not support JMAP Mail."),
            };
        }
        const auto account = session.accounts.find(identity.remoteAccountId);
        if (account == session.accounts.end() || !account->second.accountCapabilities.mail)
        {
            co_return OperationError{
                .code = OperationErrorCode::UnsupportedCapability,
                .message = QStringLiteral("This account does not support JMAP Mail."),
            };
        }

        javelin::jmap::cache::MailboxRepository repository{*m_impl->databaseConnection};
        auto found = repository.find(accountId, mailboxId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&found))
            co_return operationError(*error);
        const auto& mailbox = std::get<std::optional<javelin::jmap::domain::Mailbox>>(found);
        if (!mailbox.has_value())
        {
            co_return OperationError{
                .code = OperationErrorCode::NotFound,
                .message = QStringLiteral("The mailbox is no longer available."),
            };
        }
        if (mailbox->isSubscribed == subscribed)
        {
            co_return MailboxSubscriptionChange{
                .accountId = std::move(accountId),
                .mailboxId = std::move(mailboxId),
                .subscribed = subscribed,
            };
        }

        javelin::jmap::sync::MailboxMutationJournal journal{*m_impl->databaseConnection,
                                                            repository};
        const auto active = journal.hasActive(accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&active))
            co_return operationError(*error);
        if (std::get<bool>(active))
        {
            co_return OperationError{
                .code = OperationErrorCode::Conflict,
                .message = QStringLiteral("Another mailbox change is still unresolved."),
            };
        }

        javelin::jmap::cache::SyncStateRepository states{*m_impl->databaseConnection};
        auto state = states.find({.accountId = accountId, .objectType = "Mailbox", .queryKey = {}});
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&state))
            co_return operationError(*error);
        const auto& stateRecord =
            std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(state);
        if (!stateRecord.has_value())
        {
            co_return OperationError{
                .code = OperationErrorCode::PreconditionFailed,
                .message = QStringLiteral("Mailbox state has not been synchronized yet."),
            };
        }

        javelin::jmap::sync::MailboxSubscriptionMutationRecord mutation{
            .mutationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
            .operationGroupId = std::nullopt,
            .accountId = accountId,
            .mailboxId = mailboxId,
            .status = javelin::jmap::sync::MutationStatus::Pending,
            .beforeSubscribed = mailbox->isSubscribed,
            .afterSubscribed = subscribed,
            .baseState = stateRecord->stateToken,
            .acceptedState = std::nullopt,
            .errorJson = std::nullopt,
        };
        if (const auto error = journal.queue(mutation))
            co_return operationError(*error);
        if (projectionCommitted)
            projectionCommitted();

        auto result = co_await submitMailboxSubscriptionMutation(
            *m_impl->methodTransport, settings, accountId, identity.remoteAccountId, session,
            journal, mutation);
        if (const auto* error = std::get_if<OperationError>(&result))
            co_return *error;
        co_return MailboxSubscriptionChange{
            .accountId = std::move(accountId),
            .mailboxId = std::move(mailboxId),
            .subscribed = subscribed,
        };
    }

    QCoro::Task<MailboxSubscriptionChangeResult>
    MailboxMutationEngine::reconcileSubscription(LiveConnectionSettings settings,
                                                 std::string accountId)
    {
        if (m_impl->databaseConnection == nullptr || m_impl->methodTransport == nullptr)
        {
            co_return OperationError{
                .message = QStringLiteral("Required mail capability dependencies are unavailable."),
            };
        }
        if (const auto validationError = validateLoginSettings(settings, true))
            co_return *validationError;
        const auto identityResult =
            resolveCachedAccountIdentity(*m_impl->databaseConnection, accountId);
        if (const auto* error = std::get_if<OperationError>(&identityResult))
            co_return *error;
        const auto& identity = std::get<CachedAccountIdentity>(identityResult);
        const auto sessionResult =
            loadCachedSession(*m_impl->databaseConnection, identity.localAccountId);
        if (const auto* error = std::get_if<OperationError>(&sessionResult))
            co_return *error;
        const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);

        javelin::jmap::cache::MailboxRepository repository{*m_impl->databaseConnection};
        javelin::jmap::sync::MailboxMutationJournal journal{*m_impl->databaseConnection,
                                                            repository};
        auto activeResult = journal.listActive(accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&activeResult))
            co_return operationError(*error);
        auto active = std::get<std::vector<javelin::jmap::sync::MailboxSubscriptionMutationRecord>>(
            std::move(activeResult));
        if (active.empty())
        {
            co_return MailboxSubscriptionChange{
                .accountId = std::move(accountId), .mailboxId = {}, .subscribed = true};
        }
        if (active.size() != 1)
        {
            co_return OperationError{
                .code = OperationErrorCode::Conflict,
                .message = QStringLiteral("Multiple mailbox visibility changes are unresolved."),
            };
        }
        auto mutation = std::move(active.front());
        if (mutation.status == javelin::jmap::sync::MutationStatus::InFlight)
        {
            co_return OperationError{
                .code = OperationErrorCode::Conflict,
                .message = QStringLiteral("A mailbox visibility change is still in flight."),
            };
        }

        if (mutation.status == javelin::jmap::sync::MutationStatus::Unknown)
        {
            auto fetched = co_await fetchMailboxForReconciliation(
                *m_impl->methodTransport, settings, accountId, identity.remoteAccountId, session,
                mutation.mailboxId);
            if (const auto* error = std::get_if<OperationError>(&fetched))
                co_return *error;
            auto response = std::get<javelin::jmap::api::MailboxGetResponse>(std::move(fetched));
            if (response.accountId != identity.remoteAccountId)
            {
                co_return OperationError{
                    .code = OperationErrorCode::ProtocolViolation,
                    .message =
                        QStringLiteral("The server returned Mailbox/get for another account."),
                };
            }
            const auto mailbox = std::ranges::find(response.list, mutation.mailboxId,
                                                   &javelin::jmap::domain::Mailbox::id);
            if (mailbox == response.list.end())
            {
                if (const auto error = journal.reject(mutation))
                    co_return operationError(*error);
                co_return OperationError{
                    .code = OperationErrorCode::NotFound,
                    .message = QStringLiteral("The mailbox is no longer available."),
                };
            }
            if (mailbox->isSubscribed == mutation.afterSubscribed)
            {
                if (const auto error =
                        journal.reconcile(mutation, mailbox->isSubscribed, response.state))
                    co_return operationError(*error);
                co_return MailboxSubscriptionChange{
                    .accountId = std::move(accountId),
                    .mailboxId = std::move(mutation.mailboxId),
                    .subscribed = mutation.afterSubscribed,
                };
            }
            if (!mutation.baseState.has_value() || response.state != *mutation.baseState)
            {
                if (const auto error =
                        journal.reconcile(mutation, mailbox->isSubscribed, response.state))
                    co_return operationError(*error);
                co_return MailboxSubscriptionChange{
                    .accountId = std::move(accountId),
                    .mailboxId = std::move(mutation.mailboxId),
                    .subscribed = mailbox->isSubscribed,
                };
            }
            if (const auto error =
                    journal.transition(mutation, javelin::jmap::sync::MutationStatus::Pending))
                co_return operationError(*error);
            mutation.status = javelin::jmap::sync::MutationStatus::Pending;
        }

        auto submitted = co_await submitMailboxSubscriptionMutation(
            *m_impl->methodTransport, settings, accountId, identity.remoteAccountId, session,
            journal, mutation);
        if (const auto* error = std::get_if<OperationError>(&submitted))
            co_return *error;
        co_return MailboxSubscriptionChange{
            .accountId = std::move(accountId),
            .mailboxId = std::move(mutation.mailboxId),
            .subscribed = mutation.afterSubscribed,
        };
    }

    QCoro::Task<MailboxCreateResult>
    MailboxMutationEngine::create(LiveConnectionSettings settings, std::string accountId,
                                  std::string name, std::function<void()> projectionCommitted)
    {
        co_return co_await createInParent(std::move(settings), std::move(accountId),
                                          std::move(name), std::nullopt, std::nullopt,
                                          std::move(projectionCommitted));
    }

    QCoro::Task<MailboxCreateResult>
    MailboxMutationEngine::createInParent(LiveConnectionSettings settings, std::string accountId,
                                          std::string name, std::optional<std::string> parentId,
                                          std::optional<std::string> operationGroupId,
                                          std::function<void()> projectionCommitted)
    {
        if (m_impl->databaseConnection == nullptr || m_impl->methodTransport == nullptr)
        {
            co_return OperationError{
                .message = QStringLiteral("Required mail capability dependencies are unavailable."),
            };
        }
        if (const auto validationError = validateLoginSettings(settings, true))
            co_return *validationError;
        if (name.empty())
        {
            co_return OperationError{
                .code = OperationErrorCode::InvalidUserInput,
                .message = QStringLiteral("Enter a mailbox name."),
            };
        }

        const auto identityResult =
            resolveCachedAccountIdentity(*m_impl->databaseConnection, accountId);
        if (const auto* error = std::get_if<OperationError>(&identityResult))
            co_return *error;
        const auto& identity = std::get<CachedAccountIdentity>(identityResult);
        const auto sessionResult =
            loadCachedSession(*m_impl->databaseConnection, identity.localAccountId);
        if (const auto* error = std::get_if<OperationError>(&sessionResult))
            co_return *error;
        const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);
        if (!session.capabilities.mail)
        {
            co_return OperationError{
                .code = OperationErrorCode::UnsupportedCapability,
                .message = QStringLiteral("This server does not support JMAP Mail."),
            };
        }
        const auto account = session.accounts.find(identity.remoteAccountId);
        if (account == session.accounts.end() || !account->second.accountCapabilities.mail)
        {
            co_return OperationError{
                .code = OperationErrorCode::UnsupportedCapability,
                .message = QStringLiteral("This account does not support JMAP Mail."),
            };
        }
        if (account->second.isReadOnly ||
            !account->second.accountCapabilities.mailDetails.has_value())
        {
            co_return OperationError{
                .code = OperationErrorCode::PermissionDenied,
                .message = QStringLiteral("You do not have permission to create mailboxes in this "
                                          "account."),
            };
        }

        javelin::jmap::cache::MailboxRepository repository{*m_impl->databaseConnection};
        if (!parentId.has_value())
        {
            if (!account->second.accountCapabilities.mailDetails->mayCreateTopLevelMailbox)
            {
                co_return OperationError{
                    .code = OperationErrorCode::PermissionDenied,
                    .message = QStringLiteral("You do not have permission to create top-level "
                                              "mailboxes in this account."),
                };
            }
        }
        else
        {
            const auto parent = repository.find(accountId, *parentId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&parent))
                co_return operationError(*error);
            const auto& resolvedParent =
                std::get<std::optional<javelin::jmap::domain::Mailbox>>(parent);
            if (!resolvedParent.has_value())
            {
                co_return OperationError{
                    .code = OperationErrorCode::NotFound,
                    .message = QStringLiteral("The parent mailbox no longer exists."),
                };
            }
            if (!resolvedParent->myRights.mayCreateChild)
            {
                co_return OperationError{
                    .code = OperationErrorCode::PermissionDenied,
                    .message =
                        QStringLiteral("You do not have permission to create a child mailbox "
                                       "under this mailbox."),
                };
            }
        }

        const auto siblings = repository.listByParent(
            accountId,
            parentId.has_value() ? std::optional<std::string_view>{*parentId} : std::nullopt);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&siblings))
            co_return operationError(*error);
        if (std::ranges::find(std::get<std::vector<javelin::jmap::domain::Mailbox>>(siblings), name,
                              &javelin::jmap::domain::Mailbox::name) !=
            std::get<std::vector<javelin::jmap::domain::Mailbox>>(siblings).end())
        {
            co_return OperationError{
                .code = OperationErrorCode::InvalidUserInput,
                .message = QStringLiteral("A sibling mailbox with this name already exists."),
            };
        }

        javelin::jmap::sync::MailboxMutationJournal journal{*m_impl->databaseConnection,
                                                            repository};
        const auto active = journal.hasActive(accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&active))
            co_return operationError(*error);
        if (std::get<bool>(active))
        {
            co_return OperationError{
                .code = OperationErrorCode::Conflict,
                .message = QStringLiteral("Another mailbox change is still unresolved."),
            };
        }

        javelin::jmap::cache::SyncStateRepository states{*m_impl->databaseConnection};
        auto state = states.find({.accountId = accountId, .objectType = "Mailbox", .queryKey = {}});
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&state))
            co_return operationError(*error);
        const auto& stateRecord =
            std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(state);
        if (!stateRecord.has_value())
        {
            co_return OperationError{
                .code = OperationErrorCode::PreconditionFailed,
                .message = QStringLiteral("Mailbox state has not been synchronized yet."),
            };
        }

        javelin::jmap::sync::MailboxCreateMutationRecord mutation{
            .mutationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
            .operationGroupId = std::move(operationGroupId),
            .accountId = accountId,
            .creationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
            .name = name,
            .parentId = parentId,
            .sortOrder = 0,
            .isSubscribed = true,
            .status = javelin::jmap::sync::MutationStatus::Pending,
            .baseState = stateRecord->stateToken,
            .acceptedState = std::nullopt,
            .errorJson = std::nullopt,
        };
        if (const auto error = journal.queue(mutation))
            co_return operationError(*error);
        if (projectionCommitted)
            projectionCommitted();

        auto result = co_await submitMailboxCreateMutation(*m_impl->methodTransport, settings,
                                                           accountId, identity.remoteAccountId,
                                                           session, journal, mutation);
        if (const auto* error = std::get_if<OperationError>(&result))
            co_return *error;
        auto mailbox = std::get<javelin::jmap::domain::Mailbox>(std::move(result));
        co_return MailboxCreateChange{
            .accountId = std::move(accountId),
            .mailboxId = std::move(mailbox.id),
            .name = std::move(mailbox.name),
        };
    }

    QCoro::Task<MailboxCreateResult>
    MailboxMutationEngine::reconcileCreate(LiveConnectionSettings settings, std::string accountId,
                                           std::optional<std::string> operationGroupId)
    {
        if (m_impl->databaseConnection == nullptr || m_impl->methodTransport == nullptr)
        {
            co_return OperationError{
                .message = QStringLiteral("Required mail capability dependencies are unavailable."),
            };
        }
        if (const auto validationError = validateLoginSettings(settings, true))
            co_return *validationError;
        const auto identityResult =
            resolveCachedAccountIdentity(*m_impl->databaseConnection, accountId);
        if (const auto* error = std::get_if<OperationError>(&identityResult))
            co_return *error;
        const auto& identity = std::get<CachedAccountIdentity>(identityResult);
        const auto sessionResult =
            loadCachedSession(*m_impl->databaseConnection, identity.localAccountId);
        if (const auto* error = std::get_if<OperationError>(&sessionResult))
            co_return *error;
        const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);

        javelin::jmap::cache::MailboxRepository repository{*m_impl->databaseConnection};
        javelin::jmap::sync::MailboxMutationJournal journal{*m_impl->databaseConnection,
                                                            repository};
        auto activeResult = journal.listActiveCreates(accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&activeResult))
            co_return operationError(*error);
        auto active = std::get<std::vector<javelin::jmap::sync::MailboxCreateMutationRecord>>(
            std::move(activeResult));
        if (operationGroupId.has_value())
        {
            std::erase_if(active,
                          [operationGroupId](const auto& mutation)
                          {
                              return !mutation.operationGroupId.has_value() ||
                                     *mutation.operationGroupId != *operationGroupId;
                          });
        }
        if (active.empty())
        {
            co_return MailboxCreateChange{
                .accountId = std::move(accountId), .mailboxId = {}, .name = {}};
        }
        if (active.size() != 1)
        {
            co_return OperationError{
                .code = OperationErrorCode::Conflict,
                .message = QStringLiteral("Multiple mailbox creations are unresolved."),
            };
        }
        auto mutation = std::move(active.front());
        if (mutation.status == javelin::jmap::sync::MutationStatus::InFlight)
        {
            co_return OperationError{
                .code = OperationErrorCode::Conflict,
                .message = QStringLiteral("A mailbox creation is still in flight."),
            };
        }

        if (mutation.status == javelin::jmap::sync::MutationStatus::Unknown)
        {
            auto fetched = co_await fetchAllMailboxesForReconciliation(
                *m_impl->methodTransport, settings, accountId, identity.remoteAccountId, session);
            if (const auto* error = std::get_if<OperationError>(&fetched))
                co_return *error;
            auto response = std::get<javelin::jmap::api::MailboxGetResponse>(std::move(fetched));
            if (response.accountId != identity.remoteAccountId)
            {
                co_return OperationError{
                    .code = OperationErrorCode::ProtocolViolation,
                    .message =
                        QStringLiteral("The server returned Mailbox/get for another account."),
                };
            }

            const auto matches = [&](const javelin::jmap::domain::Mailbox& mailbox)
            { return mailbox.name == mutation.name && mailbox.parentId == mutation.parentId; };
            const auto first = std::ranges::find_if(response.list, matches);
            if (first != response.list.end())
            {
                if (std::ranges::find_if(std::next(first), response.list.end(), matches) !=
                    response.list.end())
                {
                    co_return OperationError{
                        .code = OperationErrorCode::ProtocolViolation,
                        .message = QStringLiteral("The server returned duplicate sibling "
                                                  "mailboxes with the same name."),
                    };
                }
                if (const auto error =
                        journal.reconcileCreated(mutation, response.list, response.state))
                    co_return operationError(*error);
                co_return MailboxCreateChange{
                    .accountId = std::move(accountId),
                    .mailboxId = first->id,
                    .name = first->name,
                };
            }

            if (const auto error =
                    journal.retryCreateAtState(mutation, response.list, response.state))
                co_return operationError(*error);
            mutation.status = javelin::jmap::sync::MutationStatus::Pending;
            mutation.baseState = response.state;
        }

        auto submitted = co_await submitMailboxCreateMutation(*m_impl->methodTransport, settings,
                                                              accountId, identity.remoteAccountId,
                                                              session, journal, mutation);
        if (const auto* error = std::get_if<OperationError>(&submitted))
            co_return *error;
        auto mailbox = std::get<javelin::jmap::domain::Mailbox>(std::move(submitted));
        co_return MailboxCreateChange{
            .accountId = std::move(accountId),
            .mailboxId = std::move(mailbox.id),
            .name = std::move(mailbox.name),
        };
    }

    QCoro::Task<MailboxDestroyResult>
    MailboxMutationEngine::destroy(LiveConnectionSettings settings, std::string accountId,
                                   std::string mailboxId, std::function<void()> projectionCommitted)
    {
        if (m_impl->databaseConnection == nullptr || m_impl->methodTransport == nullptr)
        {
            co_return OperationError{
                .message = QStringLiteral("Required mail capability dependencies are unavailable."),
            };
        }
        if (const auto validationError = validateLoginSettings(settings, true))
            co_return *validationError;
        const auto identityResult =
            resolveCachedAccountIdentity(*m_impl->databaseConnection, accountId);
        if (const auto* error = std::get_if<OperationError>(&identityResult))
            co_return *error;
        const auto& identity = std::get<CachedAccountIdentity>(identityResult);
        const auto sessionResult =
            loadCachedSession(*m_impl->databaseConnection, identity.localAccountId);
        if (const auto* error = std::get_if<OperationError>(&sessionResult))
            co_return *error;
        const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);
        if (!session.capabilities.mail)
        {
            co_return OperationError{
                .code = OperationErrorCode::UnsupportedCapability,
                .message = QStringLiteral("This server does not support JMAP Mail."),
            };
        }
        const auto account = session.accounts.find(identity.remoteAccountId);
        if (account == session.accounts.end() || !account->second.accountCapabilities.mail)
        {
            co_return OperationError{
                .code = OperationErrorCode::UnsupportedCapability,
                .message = QStringLiteral("This account does not support JMAP Mail."),
            };
        }

        javelin::jmap::cache::MailboxRepository repository{*m_impl->databaseConnection};
        auto found = repository.find(accountId, mailboxId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&found))
            co_return operationError(*error);
        const auto& mailbox = std::get<std::optional<javelin::jmap::domain::Mailbox>>(found);
        if (!mailbox.has_value())
        {
            co_return OperationError{
                .code = OperationErrorCode::NotFound,
                .message = QStringLiteral("The mailbox is no longer available."),
            };
        }
        if (!mailbox->myRights.mayDelete)
        {
            co_return OperationError{
                .code = OperationErrorCode::PermissionDenied,
                .message = QStringLiteral("You do not have permission to delete this mailbox."),
            };
        }
        if (mailbox->totalEmails != 0)
        {
            co_return OperationError{
                .code = OperationErrorCode::PreconditionFailed,
                .message = QStringLiteral("Empty the mailbox before deleting it."),
            };
        }
        const auto children = repository.listByParent(accountId, mailboxId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&children))
            co_return operationError(*error);
        if (!std::get<std::vector<javelin::jmap::domain::Mailbox>>(children).empty())
        {
            co_return OperationError{
                .code = OperationErrorCode::PreconditionFailed,
                .message = QStringLiteral("Delete or move child mailboxes before deleting it."),
            };
        }

        javelin::jmap::sync::MailboxMutationJournal journal{*m_impl->databaseConnection,
                                                            repository};
        const auto active = journal.hasActive(accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&active))
            co_return operationError(*error);
        if (std::get<bool>(active))
        {
            co_return OperationError{
                .code = OperationErrorCode::Conflict,
                .message = QStringLiteral("Another mailbox change is still unresolved."),
            };
        }

        javelin::jmap::cache::SyncStateRepository states{*m_impl->databaseConnection};
        auto state = states.find({.accountId = accountId, .objectType = "Mailbox", .queryKey = {}});
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&state))
            co_return operationError(*error);
        const auto& stateRecord =
            std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(state);
        if (!stateRecord.has_value())
        {
            co_return OperationError{
                .code = OperationErrorCode::PreconditionFailed,
                .message = QStringLiteral("Mailbox state has not been synchronized yet."),
            };
        }

        javelin::jmap::sync::MailboxDestroyMutationRecord mutation{
            .mutationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
            .operationGroupId = std::nullopt,
            .accountId = accountId,
            .mailboxId = mailboxId,
            .status = javelin::jmap::sync::MutationStatus::Pending,
            .beforeMailbox = *mailbox,
            .baseState = stateRecord->stateToken,
            .acceptedState = std::nullopt,
            .errorJson = std::nullopt,
        };
        if (const auto error = journal.queue(mutation))
            co_return operationError(*error);
        if (projectionCommitted)
            projectionCommitted();

        auto result = co_await submitMailboxDestroyMutation(*m_impl->methodTransport, settings,
                                                            accountId, identity.remoteAccountId,
                                                            session, journal, mutation);
        if (const auto* error = std::get_if<OperationError>(&result))
            co_return *error;
        co_return MailboxDestroyChange{.accountId = std::move(accountId),
                                       .mailboxId = std::move(mailboxId)};
    }

    QCoro::Task<MailboxDestroyResult>
    MailboxMutationEngine::reconcileDestroy(LiveConnectionSettings settings, std::string accountId)
    {
        if (m_impl->databaseConnection == nullptr || m_impl->methodTransport == nullptr)
        {
            co_return OperationError{
                .message = QStringLiteral("Required mail capability dependencies are unavailable."),
            };
        }
        if (const auto validationError = validateLoginSettings(settings, true))
            co_return *validationError;
        const auto identityResult =
            resolveCachedAccountIdentity(*m_impl->databaseConnection, accountId);
        if (const auto* error = std::get_if<OperationError>(&identityResult))
            co_return *error;
        const auto& identity = std::get<CachedAccountIdentity>(identityResult);
        const auto sessionResult =
            loadCachedSession(*m_impl->databaseConnection, identity.localAccountId);
        if (const auto* error = std::get_if<OperationError>(&sessionResult))
            co_return *error;
        const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);

        javelin::jmap::cache::MailboxRepository repository{*m_impl->databaseConnection};
        javelin::jmap::sync::MailboxMutationJournal journal{*m_impl->databaseConnection,
                                                            repository};
        auto activeResult = journal.listActiveDestroys(accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&activeResult))
            co_return operationError(*error);
        auto active = std::get<std::vector<javelin::jmap::sync::MailboxDestroyMutationRecord>>(
            std::move(activeResult));
        if (active.empty())
        {
            co_return MailboxDestroyChange{.accountId = std::move(accountId), .mailboxId = {}};
        }
        if (active.size() != 1)
        {
            co_return OperationError{
                .code = OperationErrorCode::Conflict,
                .message = QStringLiteral("Multiple mailbox deletions are unresolved."),
            };
        }
        auto mutation = std::move(active.front());
        if (mutation.status == javelin::jmap::sync::MutationStatus::InFlight)
        {
            co_return OperationError{
                .code = OperationErrorCode::Conflict,
                .message = QStringLiteral("A mailbox deletion is still in flight."),
            };
        }

        if (mutation.status == javelin::jmap::sync::MutationStatus::Unknown)
        {
            auto fetched = co_await fetchMailboxForReconciliation(
                *m_impl->methodTransport, settings, accountId, identity.remoteAccountId, session,
                mutation.mailboxId);
            if (const auto* error = std::get_if<OperationError>(&fetched))
                co_return *error;
            auto response = std::get<javelin::jmap::api::MailboxGetResponse>(std::move(fetched));
            if (response.accountId != identity.remoteAccountId)
            {
                co_return OperationError{
                    .code = OperationErrorCode::ProtocolViolation,
                    .message =
                        QStringLiteral("The server returned Mailbox/get for another account."),
                };
            }
            const auto mailbox = std::ranges::find(response.list, mutation.mailboxId,
                                                   &javelin::jmap::domain::Mailbox::id);
            if (mailbox == response.list.end())
            {
                if (const auto error = journal.reconcileDestroyed(mutation, response.state))
                    co_return operationError(*error);
                co_return MailboxDestroyChange{
                    .accountId = std::move(accountId),
                    .mailboxId = std::move(mutation.mailboxId),
                };
            }
            if (!mutation.baseState.has_value() || response.state != *mutation.baseState)
            {
                if (const auto error = journal.reconcilePresent(mutation, *mailbox, response.state))
                    co_return operationError(*error);
                co_return MailboxDestroyChange{
                    .accountId = std::move(accountId),
                    .mailboxId = std::move(mutation.mailboxId),
                };
            }
            if (const auto error =
                    journal.transition(mutation, javelin::jmap::sync::MutationStatus::Pending))
                co_return operationError(*error);
            mutation.status = javelin::jmap::sync::MutationStatus::Pending;
        }

        auto submitted = co_await submitMailboxDestroyMutation(*m_impl->methodTransport, settings,
                                                               accountId, identity.remoteAccountId,
                                                               session, journal, mutation);
        if (const auto* error = std::get_if<OperationError>(&submitted))
            co_return *error;
        co_return MailboxDestroyChange{.accountId = std::move(accountId),
                                       .mailboxId = std::move(mutation.mailboxId)};
    }

    QCoro::Task<MessageSearchResult> MailQueryMaterializer::searchMessages(
        LiveConnectionSettings settings, std::string accountId, std::string query,
        const std::size_t offset, const std::size_t limit, javelin::jmap::query::EmailListSort sort,
        std::optional<std::string> anchor, std::optional<std::string> windowKey,
        std::function<void(const QString&)> progressCallback)
    {
        co_return co_await searchMessages(
            std::move(settings), std::move(accountId),
            javelin::jmap::search::EmailSearchCriteria{.text = std::move(query)}, offset, limit,
            std::move(sort), std::move(anchor), std::move(windowKey), std::move(progressCallback),
            {});
    }

    QCoro::Task<MessageSearchResult> MailQueryMaterializer::searchMessages(
        LiveConnectionSettings settings, std::string accountId,
        javelin::jmap::search::EmailSearchCriteria criteria, const std::size_t offset,
        const std::size_t limit, javelin::jmap::query::EmailListSort sort,
        std::optional<std::string> anchor, std::optional<std::string> windowKey,
        std::function<void(const QString&)> progressCallback,
        javelin::jmap::search::EmailSearchResolution resolution)
    {
        const auto reportProgress = [&progressCallback](const QString& message)
        {
            if (progressCallback)
            {
                progressCallback(message);
            }
        };

        auto query = javelin::jmap::search::displayString(criteria);
        const auto queryKey = windowKey.value_or(javelin::jmap::search::cacheKey(criteria, sort));
        qInfo() << "Mail search materialization start"
                << "offset=" << static_cast<qulonglong>(offset)
                << "limit=" << static_cast<qulonglong>(limit) << "anchored=" << anchor.has_value()
                << "explicitWindowKey=" << windowKey.has_value();
        reportProgress(QStringLiteral("Searching the server..."));
        if (m_impl->databaseConnection == nullptr || m_impl->queryClient == nullptr)
        {
            co_return OperationError{
                .message = QStringLiteral("Mail query materialization is unavailable."),
            };
        }

        if (javelin::jmap::search::isEmpty(criteria))
        {
            co_return OperationError{
                .message = QStringLiteral("Enter a search term before searching."),
            };
        }

        const auto pageResult = co_await m_impl->queryClient->queryCollapsedPage(
            settings, accountId, javelin::jmap::search::toEmailQueryFilter(criteria, resolution),
            offset, limit, std::move(sort), std::move(anchor), 1, reportProgress);
        if (const auto* error = std::get_if<OperationError>(&pageResult))
        {
            co_return *error;
        }

        auto page = std::get<CollapsedQueryPage>(std::move(pageResult));
        auto emailIds = page.representativeIds;
        auto transactionResult = javelin::jmap::sync::MutationProjectionTransaction::begin(
            *m_impl->databaseConnection, QStringLiteral("Materialize search window"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            co_return javelin::jmap::operationError(*error);
        auto transaction = std::get<javelin::jmap::sync::MutationProjectionTransaction>(
            std::move(transactionResult));
        javelin::jmap::cache::EmailRepository emailRepository{*m_impl->databaseConnection};
        if (const auto error = emailRepository.upsertMany(transaction.cacheTransaction(), accountId,
                                                          page.representatives))
            co_return javelin::jmap::operationError(*error);
        if (const auto error = javelin::jmap::sync::rebaseActiveEmailProjections(
                transaction, *m_impl->databaseConnection, accountId, emailIds, page.emailState))
            co_return *error;
        javelin::jmap::cache::SearchWindowRepository searchWindowRepository{
            *m_impl->databaseConnection};
        if (const auto error = searchWindowRepository.replace(
                transaction.cacheTransaction(), {
                                                    .accountId = accountId,
                                                    .queryKey = queryKey,
                                                    .offset = offset,
                                                    .limit = limit,
                                                    .position = page.position,
                                                    .returnedLimit = page.returnedLimit,
                                                    .total = page.total,
                                                    .queryState = page.queryState,
                                                    .emailIds = emailIds,
                                                }))
            co_return javelin::jmap::operationError(*error);
        if (const auto error = transaction.commit())
            co_return javelin::jmap::operationError(*error);

        javelin::jmap::cache::MessageSummaryReadRepository messageSummaries{
            *m_impl->databaseConnection};
        const auto cachedResults = messageSummaries.listMessagesByEmailIds(accountId, emailIds);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&cachedResults))
        {
            co_return javelin::jmap::operationError(*error);
        }
        auto results = std::get<std::vector<javelin::jmap::cache::MessageListItem>>(cachedResults);

        co_return MessageSearchSummary{
            .accountId = std::move(accountId),
            .query = std::move(query),
            .offset = offset,
            .limit = limit,
            .position = page.position,
            .returnedLimit = page.returnedLimit,
            .representativeCount = page.representativeCount,
            .total = page.total,
            .queryState = std::move(page.queryState),
            .results = std::move(results),
        };
    }

    QCoro::Task<MailboxPageResult> MailQueryMaterializer::queryMailboxPage(
        LiveConnectionSettings settings, std::string accountId, std::string mailboxId,
        const std::size_t offset, const std::size_t limit, javelin::jmap::query::EmailListSort sort,
        std::optional<std::string> anchor, const std::int64_t anchorOffset,
        std::function<void(const QString&)> progressCallback)
    {
        const auto reportProgress = [&progressCallback](const QString& message)
        {
            if (progressCallback)
            {
                progressCallback(message);
            }
        };

        qInfo() << "Mailbox page materialization"
                << "offset=" << static_cast<qulonglong>(offset)
                << "limit=" << static_cast<qulonglong>(limit) << "anchored=" << anchor.has_value();
        reportProgress(QStringLiteral("Fetching mailbox page from the server..."));
        if (m_impl->databaseConnection == nullptr || m_impl->queryClient == nullptr)
        {
            co_return OperationError{
                .message = QStringLiteral("Mail query materialization is unavailable."),
            };
        }

        const bool anchoredRequest = anchor.has_value();
        const auto pageResult = co_await m_impl->queryClient->queryCollapsedPage(
            settings, accountId,
            javelin::jmap::api::EmailQueryFilter{
                .inMailbox = mailboxId,
                .text = std::nullopt,
            },
            offset, limit, std::move(sort), std::move(anchor), anchorOffset, reportProgress);
        if (const auto* error = std::get_if<OperationError>(&pageResult))
        {
            co_return *error;
        }

        auto page = std::get<CollapsedQueryPage>(std::move(pageResult));
        const auto materializedOffset = javelin::jmap::sync::materializedMailboxWindowOffset(
            offset, anchoredRequest, page.position);
        auto representativeIds = page.representativeIds;
        const auto queryKey = javelin::jmap::sync::mailboxQueryKey({
            .mailboxId = mailboxId,
            .sortProperty = javelin::jmap::query::propertyName(sort.property),
            .isAscending = javelin::jmap::query::isAscending(sort),
            .collapseThreads = true,
        });
        auto transactionResult = javelin::jmap::sync::MutationProjectionTransaction::begin(
            *m_impl->databaseConnection, QStringLiteral("Materialize mailbox window"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            co_return javelin::jmap::operationError(*error);
        auto transaction = std::get<javelin::jmap::sync::MutationProjectionTransaction>(
            std::move(transactionResult));
        javelin::jmap::cache::EmailRepository emailRepository{*m_impl->databaseConnection};
        if (const auto error = emailRepository.upsertMany(transaction.cacheTransaction(), accountId,
                                                          page.representatives))
            co_return javelin::jmap::operationError(*error);
        if (const auto error = javelin::jmap::sync::rebaseActiveEmailProjections(
                transaction, *m_impl->databaseConnection, accountId, representativeIds,
                page.emailState))
            co_return *error;
        javelin::jmap::cache::MailboxWindowRepository windowRepository{*m_impl->databaseConnection};
        if (const auto error = windowRepository.replace(
                transaction.cacheTransaction(), {
                                                    .accountId = accountId,
                                                    .mailboxId = mailboxId,
                                                    .queryKey = queryKey,
                                                    .requestedOffset = materializedOffset,
                                                    .requestedLimit = limit,
                                                    .position = page.position,
                                                    .returnedLimit = page.returnedLimit,
                                                    .total = page.total,
                                                    .queryState = page.queryState,
                                                    .emailIds = std::move(representativeIds),
                                                }))
            co_return javelin::jmap::operationError(*error);
        if (const auto error = transaction.commit())
            co_return javelin::jmap::operationError(*error);

        javelin::jmap::cache::MessageSummaryReadRepository messageSummaries{
            *m_impl->databaseConnection};
        const auto cachedResults =
            messageSummaries.listMessagesByEmailIds(accountId, page.representativeIds);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&cachedResults))
            co_return javelin::jmap::operationError(*error);
        auto results = std::get<std::vector<javelin::jmap::cache::MessageListItem>>(cachedResults);
        co_return MailboxPageSummary{
            .accountId = std::move(accountId),
            .mailboxId = std::move(mailboxId),
            .offset = materializedOffset,
            .limit = limit,
            .position = page.position,
            .returnedLimit = page.returnedLimit,
            .representativeCount = page.representativeCount,
            .total = page.total,
            .queryState = std::move(page.queryState),
            .results = std::move(results),
        };
    }

} // namespace javelin::jmap
