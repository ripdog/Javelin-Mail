#include "jmap/JmapCore.h"

#include "jmap/api/Error.h"
#include "jmap/api/MailMethods.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/MethodEnvelope.h"
#include "jmap/api/RequestBuilder.h"
#include "jmap/api/ResponseReader.h"
#include "jmap/api/SessionClient.h"
#include "jmap/api/Transport.h"
#include "jmap/auth/AccessTokenResolver.h"
#include "jmap/auth/Auth.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/MessageContentTypes.h"
#include "jmap/cache/MimeMessageParser.h"
#include "jmap/cache/RawMessageSourceRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/cache/ThreadRepository.h"
#include "jmap/sync/MailboxRefreshExecutor.h"
#include "jmap/sync/PendingActions.h"

#include <QDebug>
#include <QSqlError>
#include <QSqlQuery>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <algorithm>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace javelin::jmap
{
    struct JmapCore::Impl
    {
        javelin::jmap::cache::DatabaseConnection* databaseConnection = nullptr;
        javelin::jmap::api::AbstractTransport* transport = nullptr;
        QString statusSummary = QStringLiteral(
            "JMAP core scaffolded. Session discovery and typed protocol live here next.");
    };

    namespace
    {

        [[nodiscard]] QString transportMessage(const javelin::jmap::api::TransportError& error)
        {
            const auto code = QString::fromUtf8(javelin::jmap::api::toString(error.code).data());
            if (error.httpStatus.has_value())
            {
                return QStringLiteral("Transport error (%1, HTTP %2): %3")
                    .arg(code)
                    .arg(*error.httpStatus)
                    .arg(QString::fromStdString(error.message));
            }

            return QStringLiteral("Transport error (%1): %2")
                .arg(code, QString::fromStdString(error.message));
        }

        [[nodiscard]] QString authMessage(const javelin::jmap::api::AuthError& error)
        {
            return QStringLiteral("Authentication error (%1): %2")
                .arg(QString::fromUtf8(javelin::jmap::api::toString(error.code).data()),
                     QString::fromStdString(error.message));
        }

        [[nodiscard]] QString protocolMessage(const javelin::jmap::api::ProtocolError& error)
        {
            return QStringLiteral("Protocol error (%1): %2")
                .arg(QString::fromUtf8(javelin::jmap::api::toString(error.code).data()),
                     QString::fromStdString(error.message));
        }

        struct DownloadContext
        {
            javelin::jmap::auth::AccountCredentials credentials;
            javelin::jmap::api::Session session;
            std::string accessToken;
        };

        struct BlobDownloadError
        {
            LiveRefreshError error;
            std::optional<int> httpStatus;
        };

        [[nodiscard]] std::optional<std::string>
        selectMailboxForInitialSync(const std::vector<javelin::jmap::domain::Mailbox>& mailboxes)
        {
            for (const auto& mailbox : mailboxes)
            {
                if (mailbox.role == std::optional<std::string>{"inbox"})
                {
                    return mailbox.id;
                }
            }

            if (!mailboxes.empty())
            {
                return mailboxes.front().id;
            }

            return std::nullopt;
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
        buildDownloadRequest(const QUrl& url, const std::string& accessToken)
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

        [[nodiscard]] std::optional<LiveRefreshError>
        validateLoginSettings(const LiveConnectionSettings& settings, const bool requireSessionUrl)
        {
            if ((requireSessionUrl && settings.sessionUrl.empty()) || settings.loginEmail.empty() ||
                settings.apiKey.empty())
            {
                return LiveRefreshError{
                    .message =
                        requireSessionUrl
                            ? QStringLiteral("Session URL, login email, and API key are required.")
                            : QStringLiteral("Login email and API key are required."),
                    .requiresUserIntervention = true,
                };
            }

            return std::nullopt;
        }

        [[nodiscard]] std::variant<javelin::jmap::api::Session, LiveRefreshError>
        loadCachedSession(javelin::jmap::cache::DatabaseConnection& connection,
                          const std::string_view accountId)
        {
            javelin::jmap::cache::SessionRepository sessionRepository{connection};
            const auto sessionResult = sessionRepository.load(accountId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&sessionResult))
            {
                return LiveRefreshError{.message = error->message};
            }

            const auto& session =
                std::get<std::optional<javelin::jmap::api::Session>>(sessionResult);
            if (!session.has_value())
            {
                return LiveRefreshError{
                    .message =
                        QStringLiteral("No cached JMAP session is available for this account."),
                };
            }

            return *session;
        }

        [[nodiscard]] std::variant<std::string, LiveRefreshError>
        resolveAccessToken(const javelin::jmap::auth::AccountCredentials& credentials)
        {
            const javelin::jmap::auth::AccessTokenResolver accessTokenResolver;
            const auto tokenResult = accessTokenResolver.resolve(credentials);
            if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&tokenResult))
            {
                return LiveRefreshError{
                    .message = authMessage(*error),
                    .requiresUserIntervention = true,
                };
            }

            return std::get<javelin::jmap::auth::OAuthToken>(tokenResult).accessToken;
        }

        [[nodiscard]] std::variant<DownloadContext, LiveRefreshError>
        prepareDownloadContext(javelin::jmap::cache::DatabaseConnection& connection,
                               const LiveConnectionSettings& settings, const std::string& accountId)
        {
            if (const auto validationError = validateLoginSettings(settings, true))
            {
                return *validationError;
            }

            auto credentials = buildAccountCredentials(settings, accountId);
            const auto sessionResult = loadCachedSession(connection, accountId);
            if (const auto* error = std::get_if<LiveRefreshError>(&sessionResult))
            {
                return *error;
            }

            const auto tokenResult = resolveAccessToken(credentials);
            if (const auto* error = std::get_if<LiveRefreshError>(&tokenResult))
            {
                return *error;
            }

            return DownloadContext{
                .credentials = std::move(credentials),
                .session = std::get<javelin::jmap::api::Session>(sessionResult),
                .accessToken = std::get<std::string>(tokenResult),
            };
        }

        [[nodiscard]] QCoro::Task<std::variant<QByteArray, BlobDownloadError>>
        downloadBlob(javelin::jmap::api::AbstractTransport& transport,
                     std::string downloadUrlTemplate, std::string accountId,
                     javelin::jmap::cache::EmailPart part, std::string accessToken,
                     QString failurePrefix)
        {
            const auto transportResult = co_await transport.send(buildDownloadRequest(
                buildDownloadUrl(downloadUrlTemplate, accountId, part), accessToken));
            if (const auto* error =
                    std::get_if<javelin::jmap::api::TransportError>(&transportResult))
            {
                co_return BlobDownloadError{
                    .error = LiveRefreshError{.message = transportMessage(*error)},
                    .httpStatus = error->httpStatus,
                };
            }

            const auto& response = std::get<javelin::jmap::api::HttpResponse>(transportResult);
            if (response.statusCode < 200 || response.statusCode >= 300)
            {
                co_return BlobDownloadError{
                    .error =
                        LiveRefreshError{
                            .message = QStringLiteral("%1 failed with HTTP status %2.")
                                           .arg(failurePrefix)
                                           .arg(response.statusCode),
                        },
                    .httpStatus = response.statusCode,
                };
            }

            co_return response.body;
        }

        [[nodiscard]] std::variant<javelin::jmap::domain::Email, LiveRefreshError>
        findEmailForDownload(javelin::jmap::cache::DatabaseConnection& connection,
                             const std::string_view accountId, const std::string_view emailId)
        {
            javelin::jmap::cache::EmailRepository emailRepository{connection};
            const auto emailResult = emailRepository.find(accountId, emailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&emailResult))
            {
                return LiveRefreshError{.message = error->message};
            }

            const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(emailResult);
            if (!email.has_value())
            {
                return LiveRefreshError{
                    .message = QStringLiteral("The selected message is not cached locally."),
                };
            }

            if (email->blobId.empty())
            {
                return LiveRefreshError{
                    .message = QStringLiteral("The selected message does not expose a blob id."),
                };
            }

            return *email;
        }

        [[nodiscard]] QueuedEmailMutationResult
        queueMailboxPatch(javelin::jmap::cache::DatabaseConnection& connection,
                          std::string accountId, std::string emailId, std::string sourceMailboxId,
                          std::string destinationMailboxId, const bool removeSourceMailbox)
        {
            if (sourceMailboxId.empty() || destinationMailboxId.empty())
            {
                return LiveRefreshError{
                    .message = QStringLiteral("Source and destination mailbox ids are required."),
                };
            }
            if (sourceMailboxId == destinationMailboxId)
            {
                return LiveRefreshError{
                    .message =
                        QStringLiteral("Source and destination mailboxes must be different."),
                };
            }

            javelin::jmap::cache::EmailRepository emailRepository{connection};
            const auto emailResult = emailRepository.find(accountId, emailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&emailResult))
            {
                return LiveRefreshError{.message = error->message};
            }

            const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(emailResult);
            if (!email.has_value())
            {
                return LiveRefreshError{
                    .message = QStringLiteral("The selected message is not cached locally."),
                };
            }

            const auto pendingActionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const javelin::jmap::sync::PendingActionRecord pendingAction{
                .pendingActionId = pendingActionId.toStdString(),
                .accountId = accountId,
                .status = javelin::jmap::sync::PendingActionStatus::Pending,
                .emailPatch =
                    {
                        .emailId = emailId,
                        .addMailboxIds = {destinationMailboxId},
                        .removeMailboxIds = removeSourceMailbox
                                                ? std::vector<std::string>{sourceMailboxId}
                                                : std::vector<std::string>{},
                        .addKeywords = {},
                        .removeKeywords = {},
                    },
            };

            javelin::jmap::sync::PendingActionRepository pendingActionRepository{connection};
            if (const auto error = pendingActionRepository.put(pendingAction))
            {
                return LiveRefreshError{.message = error->message};
            }

            const auto reconciledEmail =
                javelin::jmap::sync::mergePendingEmailPatch(*email, {pendingAction});
            if (const auto error = emailRepository.upsertMany(accountId, {reconciledEmail}))
            {
                return LiveRefreshError{.message = error->message};
            }

            return QueuedEmailMutation{
                .pendingActionId = pendingActionId.toStdString(),
                .accountId = std::move(accountId),
                .emailId = std::move(emailId),
            };
        }

        [[nodiscard]] QueuedEmailMutationResult
        queueDestroyEmailMutation(javelin::jmap::cache::DatabaseConnection& connection,
                                  std::string accountId, std::string emailId)
        {
            javelin::jmap::cache::EmailRepository emailRepository{connection};
            const auto emailResult = emailRepository.find(accountId, emailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&emailResult))
            {
                return LiveRefreshError{.message = error->message};
            }

            const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(emailResult);
            if (!email.has_value())
            {
                return LiveRefreshError{
                    .message = QStringLiteral("The selected message is not cached locally."),
                };
            }

            const auto pendingActionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const javelin::jmap::sync::PendingActionRecord pendingAction{
                .pendingActionId = pendingActionId.toStdString(),
                .accountId = accountId,
                .status = javelin::jmap::sync::PendingActionStatus::Pending,
                .emailPatch =
                    {
                        .emailId = emailId,
                        .addMailboxIds = {},
                        .removeMailboxIds = email->mailboxIds,
                        .addKeywords = {},
                        .removeKeywords = {},
                        .destroy = true,
                    },
            };

            javelin::jmap::sync::PendingActionRepository pendingActionRepository{connection};
            if (const auto error = pendingActionRepository.put(pendingAction))
            {
                return LiveRefreshError{.message = error->message};
            }

            const auto reconciledEmail =
                javelin::jmap::sync::mergePendingEmailPatch(*email, {pendingAction});
            if (const auto error = emailRepository.upsertMany(accountId, {reconciledEmail}))
            {
                return LiveRefreshError{.message = error->message};
            }

            return QueuedEmailMutation{
                .pendingActionId = pendingActionId.toStdString(),
                .accountId = std::move(accountId),
                .emailId = std::move(emailId),
            };
        }

        [[nodiscard]] QueuedEmailMutationResult
        queueKeywordPatch(javelin::jmap::cache::DatabaseConnection& connection,
                          std::string accountId, std::string emailId, std::string keyword,
                          const bool enabled)
        {
            if (keyword.empty())
            {
                return LiveRefreshError{
                    .message = QStringLiteral("A keyword id is required."),
                };
            }

            javelin::jmap::cache::EmailRepository emailRepository{connection};
            const auto emailResult = emailRepository.find(accountId, emailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&emailResult))
            {
                return LiveRefreshError{.message = error->message};
            }

            const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(emailResult);
            if (!email.has_value())
            {
                return LiveRefreshError{
                    .message = QStringLiteral("The selected message is not cached locally."),
                };
            }

            const auto pendingActionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const javelin::jmap::sync::PendingActionRecord pendingAction{
                .pendingActionId = pendingActionId.toStdString(),
                .accountId = accountId,
                .status = javelin::jmap::sync::PendingActionStatus::Pending,
                .emailPatch =
                    {
                        .emailId = emailId,
                        .addMailboxIds = {},
                        .removeMailboxIds = {},
                        .addKeywords = enabled ? std::vector<std::string>{keyword}
                                               : std::vector<std::string>{},
                        .removeKeywords = enabled ? std::vector<std::string>{}
                                                  : std::vector<std::string>{keyword},
                    },
            };

            javelin::jmap::sync::PendingActionRepository pendingActionRepository{connection};
            if (const auto error = pendingActionRepository.put(pendingAction))
            {
                return LiveRefreshError{.message = error->message};
            }

            const auto reconciledEmail =
                javelin::jmap::sync::mergePendingEmailPatch(*email, {pendingAction});
            if (const auto error = emailRepository.upsertMany(accountId, {reconciledEmail}))
            {
                return LiveRefreshError{.message = error->message};
            }

            return QueuedEmailMutation{
                .pendingActionId = pendingActionId.toStdString(),
                .accountId = std::move(accountId),
                .emailId = std::move(emailId),
            };
        }

        [[nodiscard]] std::unordered_map<std::string, javelin::jmap::api::EmailPatchValue>
        enabledMap(const std::vector<std::string>& values)
        {
            std::unordered_map<std::string, javelin::jmap::api::EmailPatchValue> enabled;
            enabled.reserve(values.size());
            for (const auto& value : values)
            {
                enabled.emplace(value, true);
            }
            return enabled;
        }

        [[nodiscard]] std::vector<javelin::jmap::sync::PendingActionRecord>
        activePendingActions(const std::vector<javelin::jmap::sync::PendingActionRecord>& actions)
        {
            std::vector<javelin::jmap::sync::PendingActionRecord> filtered;
            filtered.reserve(actions.size());
            for (const auto& action : actions)
            {
                if (action.status != javelin::jmap::sync::PendingActionStatus::Failed)
                {
                    filtered.push_back(action);
                }
            }
            return filtered;
        }

        [[nodiscard]] javelin::jmap::cache::MessageListItem
        messageListItemFromEmail(const javelin::jmap::domain::Email& email,
                                 const std::size_t threadMessageCount)
        {
            return javelin::jmap::cache::MessageListItem{
                .emailId = email.id,
                .threadId = email.threadId,
                .subject = email.subject,
                .preview = email.preview,
                .receivedAt = email.receivedAt,
                .sentAt = email.sentAt,
                .threadMessageCount = threadMessageCount,
                .hasAttachment = email.hasAttachment,
                .isUnread =
                    std::ranges::find(email.keywords, std::string{"$seen"}) == email.keywords.end(),
                .isFlagged = std::ranges::find(email.keywords, std::string{"$flagged"}) !=
                             email.keywords.end(),
                .from =
                    email.from.empty()
                        ? std::nullopt
                        : std::optional<javelin::jmap::domain::EmailAddress>{email.from.front()},
            };
        }

        [[nodiscard]] javelin::jmap::api::ApiRequestContext
        buildApiRequestContext(const LiveConnectionSettings& settings, std::string accountId,
                               const javelin::jmap::api::Session& session)
        {
            return javelin::jmap::api::ApiRequestContext{
                .credentials = buildAccountCredentials(settings, std::move(accountId)),
                .apiUrl = session.apiUrl,
            };
        }

        struct CollapsedQueryPage
        {
            std::size_t representativeCount = 0;
            std::optional<std::size_t> total;
            std::vector<javelin::jmap::cache::MessageListItem> results;
        };

        [[nodiscard]] QCoro::Task<std::variant<CollapsedQueryPage, LiveRefreshError>>
        performCollapsedQueryPage(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                                  javelin::jmap::api::AbstractTransport& transport,
                                  LiveConnectionSettings settings, std::string accountId,
                                  javelin::jmap::api::EmailQueryFilter filter,
                                  const std::size_t offset, const std::size_t limit,
                                  javelin::jmap::query::EmailListSort sort,
                                  std::function<void(const QString&)> reportProgress)
        {
            if (const auto validationError = validateLoginSettings(settings, true))
            {
                co_return *validationError;
            }

            const auto sessionResult = loadCachedSession(databaseConnection, accountId);
            if (const auto* error = std::get_if<LiveRefreshError>(&sessionResult))
            {
                co_return *error;
            }
            const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);

            javelin::jmap::api::MethodCaller methodCaller{transport};
            const auto apiRequestContext = buildApiRequestContext(settings, accountId, session);

            javelin::jmap::api::RequestBuilder builder;
            builder.useCore().useMail();

            const auto queryRequest = javelin::jmap::api::emailQuery({
                .accountId = accountId,
                .filter = filter,
                .sort = {javelin::jmap::query::toEmailQuerySort(sort)},
                .position = static_cast<std::uint64_t>(offset),
                .limit = static_cast<std::uint64_t>(limit),
                .collapseThreads = true,
                .calculateTotal = true,
            });
            if (!queryRequest.has_value())
            {
                co_return LiveRefreshError{
                    .message = QStringLiteral("Failed to encode the Email/query request."),
                };
            }
            const auto queryHandle = builder.call(*queryRequest, "page-query");

            const auto representativeRequest = javelin::jmap::api::emailGet(
                javelin::jmap::api::getRequestFrom(accountId, queryHandle, "/ids"));
            if (!representativeRequest.has_value())
            {
                co_return LiveRefreshError{
                    .message =
                        QStringLiteral("Failed to encode the representative Email/get request."),
                };
            }
            const auto representativeHandle =
                builder.call(*representativeRequest, "page-representatives-get");

            const auto threadRequest =
                javelin::jmap::api::threadGet(javelin::jmap::api::getRequestFrom(
                    accountId, representativeHandle, "/list/*/threadId"));
            if (!threadRequest.has_value())
            {
                co_return LiveRefreshError{
                    .message = QStringLiteral("Failed to encode the Thread/get request."),
                };
            }
            const auto threadHandle = builder.call(*threadRequest, "page-threads-get");

            const auto emailRequest = javelin::jmap::api::emailGet(
                javelin::jmap::api::getRequestFrom(accountId, threadHandle, "/list/*/emailIds"));
            if (!emailRequest.has_value())
            {
                co_return LiveRefreshError{
                    .message = QStringLiteral("Failed to encode the page Email/get request."),
                };
            }
            const auto emailHandle = builder.call(*emailRequest, "page-emails-get");

            const auto envelopeResult = co_await methodCaller.call(apiRequestContext, builder);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::TransportError>(&envelopeResult))
            {
                co_return LiveRefreshError{.message = transportMessage(*error)};
            }
            if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&envelopeResult))
            {
                co_return LiveRefreshError{
                    .message = authMessage(*error),
                    .requiresUserIntervention = true,
                };
            }
            if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&envelopeResult))
            {
                co_return LiveRefreshError{.message = protocolMessage(*error)};
            }

            const auto& envelope = std::get<javelin::jmap::api::ResponseEnvelope>(envelopeResult);
            const javelin::jmap::api::ResponseReader reader{envelope};

            const auto queryResult = reader.require(queryHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&queryResult))
            {
                co_return LiveRefreshError{
                    .message = QStringLiteral("Failed to read Email/query response: %1")
                                   .arg(QString::fromStdString(error->message)),
                };
            }
            const auto& parsedQuery = std::get<javelin::jmap::api::EmailQueryResponse>(queryResult);
            if (reportProgress)
            {
                reportProgress(QStringLiteral("Fetched %1 matching conversations.")
                                   .arg(static_cast<qulonglong>(parsedQuery.ids.size())));
            }

            if (parsedQuery.ids.empty())
            {
                co_return CollapsedQueryPage{
                    .representativeCount = 0,
                    .total = parsedQuery.total.has_value()
                                 ? std::optional<std::size_t>{static_cast<std::size_t>(
                                       *parsedQuery.total)}
                                 : std::nullopt,
                    .results = {},
                };
            }

            const auto representativeResult = reader.require(representativeHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&representativeResult))
            {
                co_return LiveRefreshError{
                    .message =
                        QStringLiteral("Failed to read representative Email/get response: %1")
                            .arg(QString::fromStdString(error->message)),
                };
            }
            const auto& parsedRepresentatives =
                std::get<javelin::jmap::api::EmailGetResponse>(representativeResult);

            const auto threadResult = reader.require(threadHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&threadResult))
            {
                co_return LiveRefreshError{
                    .message = QStringLiteral("Failed to read Thread/get response: %1")
                                   .arg(QString::fromStdString(error->message)),
                };
            }
            const auto& parsedThreads =
                std::get<javelin::jmap::api::ThreadGetResponse>(threadResult);

            const auto emailResult = reader.require(emailHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&emailResult))
            {
                co_return LiveRefreshError{
                    .message = QStringLiteral("Failed to read Email/get response: %1")
                                   .arg(QString::fromStdString(error->message)),
                };
            }
            const auto& parsedEmails = std::get<javelin::jmap::api::EmailGetResponse>(emailResult);

            javelin::jmap::cache::EmailRepository emailRepository{databaseConnection};
            if (const auto error = emailRepository.upsertMany(accountId, parsedEmails.list))
            {
                co_return LiveRefreshError{.message = error->message};
            }

            javelin::jmap::cache::ThreadRepository threadRepository{databaseConnection};
            if (const auto error = threadRepository.upsertMany(accountId, parsedThreads.list))
            {
                co_return LiveRefreshError{.message = error->message};
            }

            std::unordered_map<std::string, std::size_t> threadMessageCounts;
            threadMessageCounts.reserve(parsedThreads.list.size());
            for (const auto& thread : parsedThreads.list)
            {
                threadMessageCounts.emplace(thread.id, thread.emailIds.size());
            }

            std::unordered_map<std::string, const javelin::jmap::domain::Email*>
                representativesById;
            representativesById.reserve(parsedRepresentatives.list.size());
            for (const auto& email : parsedRepresentatives.list)
            {
                representativesById.emplace(email.id, &email);
            }

            std::vector<javelin::jmap::cache::MessageListItem> results;
            results.reserve(parsedQuery.ids.size());
            for (const auto& representativeId : parsedQuery.ids)
            {
                const auto representativeIt = representativesById.find(representativeId);
                if (representativeIt == representativesById.end())
                {
                    continue;
                }

                const auto* email = representativeIt->second;
                const auto threadCountIt = threadMessageCounts.find(email->threadId);
                results.push_back(messageListItemFromEmail(
                    *email,
                    threadCountIt == threadMessageCounts.end() ? 1 : threadCountIt->second));
            }

            co_return CollapsedQueryPage{
                .representativeCount = results.size(),
                .total =
                    parsedQuery.total.has_value()
                        ? std::optional<std::size_t>{static_cast<std::size_t>(*parsedQuery.total)}
                        : std::nullopt,
                .results = std::move(results),
            };
        }

    } // namespace

    JmapCore::JmapCore() : m_impl(std::make_unique<Impl>())
    {
    }

    JmapCore::JmapCore(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                       javelin::jmap::api::AbstractTransport& transport)
        : m_impl(std::make_unique<Impl>())
    {
        m_impl->databaseConnection = &databaseConnection;
        m_impl->transport = &transport;
    }

    JmapCore::~JmapCore() = default;

    QString JmapCore::statusSummary() const
    {
        return m_impl->statusSummary;
    }

    QCoro::Task<LiveRefreshResult>
    JmapCore::refreshFromServer(LiveConnectionSettings settings,
                                std::function<void(const QString&)> progressCallback)
    {
        const auto reportProgress = [&progressCallback](const QString& message)
        {
            if (progressCallback)
            {
                progressCallback(message);
            }
        };

        qInfo().noquote() << "JMAP core refresh start"
                          << QString::fromStdString(settings.loginEmail)
                          << QString::fromStdString(settings.sessionUrl);
        reportProgress(QStringLiteral("Discovering JMAP session..."));
        if (m_impl->databaseConnection == nullptr || m_impl->transport == nullptr)
        {
            co_return LiveRefreshError{
                .message =
                    QStringLiteral("Live refresh is unavailable in this process configuration."),
            };
        }

        if (const auto validationError = validateLoginSettings(settings, false))
        {
            co_return *validationError;
        }

        javelin::jmap::api::SessionClient sessionClient{*m_impl->transport};

        const javelin::jmap::auth::SessionRequestContext sessionRequestContext{
            .credentials = buildAccountCredentials(settings, settings.loginEmail),
            .requiredCapabilities =
                {
                    .mail = true,
                    .submission = false,
                },
        };

        const auto discovered = co_await sessionClient.discover(sessionRequestContext);
        if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&discovered))
        {
            co_return LiveRefreshError{.message = transportMessage(*error)};
        }
        if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&discovered))
        {
            co_return LiveRefreshError{
                .message = authMessage(*error),
                .requiresUserIntervention = true,
            };
        }
        if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&discovered))
        {
            co_return LiveRefreshError{.message = protocolMessage(*error)};
        }

        const auto& session = std::get<javelin::jmap::api::Session>(discovered);
        reportProgress(QStringLiteral("Session discovered. Saving account state..."));
        if (!session.primaryAccounts.mailAccountId.has_value())
        {
            co_return LiveRefreshError{
                .message =
                    QStringLiteral("The server session does not expose a primary mail account."),
            };
        }

        const auto& accountId = *session.primaryAccounts.mailAccountId;
        javelin::jmap::cache::SessionRepository sessionRepository{*m_impl->databaseConnection};
        qInfo().noquote() << "JMAP core saving session and accounts"
                          << QString::fromStdString(accountId)
                          << static_cast<qulonglong>(session.accounts.size());
        if (const auto error = sessionRepository.replace(accountId, session))
        {
            co_return LiveRefreshError{.message = error->message};
        }
        reportProgress(QStringLiteral("Cached session. Fetching mailboxes..."));
        const auto apiRequestContext = buildApiRequestContext(settings, accountId, session);
        qInfo().noquote() << "JMAP core mailbox request context ready"
                          << QString::fromStdString(apiRequestContext.apiUrl);

        javelin::jmap::api::MethodCaller methodCaller{*m_impl->transport};
        const auto mailboxRequest = javelin::jmap::api::mailboxGet({.accountId = accountId,
                                                                    .ids = std::nullopt,
                                                                    .idsReference = std::nullopt,
                                                                    .properties = std::nullopt});
        if (!mailboxRequest.has_value())
        {
            co_return LiveRefreshError{
                .message = QStringLiteral("Failed to encode the Mailbox/get request."),
            };
        }
        qInfo().noquote() << "JMAP core Mailbox/get request encoded"
                          << QString::fromStdString(mailboxRequest->arguments);

        javelin::jmap::api::RequestBuilder mailboxRequestBuilder;
        mailboxRequestBuilder.useCore().useMail();
        const auto mailboxHandle = mailboxRequestBuilder.call(*mailboxRequest, "mailboxes");

        const auto mailboxEnvelopeResult =
            co_await methodCaller.call(apiRequestContext, mailboxRequestBuilder);
        if (const auto* error =
                std::get_if<javelin::jmap::api::TransportError>(&mailboxEnvelopeResult))
        {
            co_return LiveRefreshError{.message = transportMessage(*error)};
        }
        if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&mailboxEnvelopeResult))
        {
            co_return LiveRefreshError{
                .message = authMessage(*error),
                .requiresUserIntervention = true,
            };
        }
        if (const auto* error =
                std::get_if<javelin::jmap::api::ProtocolError>(&mailboxEnvelopeResult))
        {
            co_return LiveRefreshError{.message = protocolMessage(*error)};
        }

        const auto& mailboxEnvelope =
            std::get<javelin::jmap::api::ResponseEnvelope>(mailboxEnvelopeResult);
        const javelin::jmap::api::ResponseReader mailboxReader{mailboxEnvelope};
        const auto mailboxResult = mailboxReader.require(mailboxHandle);
        if (const auto* error =
                std::get_if<javelin::jmap::api::ResponseReaderError>(&mailboxResult))
        {
            co_return LiveRefreshError{
                .message = QStringLiteral("Failed to read Mailbox/get response: %1")
                               .arg(QString::fromStdString(error->message)),
            };
        }
        const auto& parsedMailboxes =
            std::get<javelin::jmap::api::MailboxGetResponse>(mailboxResult);
        reportProgress(QStringLiteral("Fetched %1 mailboxes. Updating cache...")
                           .arg(parsedMailboxes.list.size()));

        javelin::jmap::cache::MailboxRepository mailboxRepository{*m_impl->databaseConnection};
        if (const auto error = mailboxRepository.replaceAll(accountId, parsedMailboxes.list))
        {
            co_return LiveRefreshError{.message = error->message};
        }

        javelin::jmap::cache::SyncStateRepository syncStateRepository{*m_impl->databaseConnection};
        if (const auto error = syncStateRepository.upsert(
                {.accountId = accountId, .objectType = "Mailbox", .queryKey = {}},
                parsedMailboxes.state))
        {
            co_return LiveRefreshError{.message = error->message};
        }

        const auto selectedMailboxId = selectMailboxForInitialSync(parsedMailboxes.list);
        std::size_t emailCount = 0;

        if (selectedMailboxId.has_value())
        {
            reportProgress(QStringLiteral("Fetching conversation list..."));
            javelin::jmap::sync::MailboxRefreshExecutor mailboxRefreshExecutor{
                *m_impl->databaseConnection, methodCaller, apiRequestContext};
            const auto refreshResult = co_await mailboxRefreshExecutor.refreshCollapsedMailbox(
                accountId, *selectedMailboxId, reportProgress);
            if (const auto* error =
                    std::get_if<javelin::jmap::sync::MailboxRefreshError>(&refreshResult))
            {
                co_return LiveRefreshError{.message = error->message};
            }

            emailCount = std::get<javelin::jmap::sync::MailboxRefreshSummary>(refreshResult)
                             .representativeCount;
            reportProgress(QStringLiteral("Cached %1 threaded conversations.").arg(emailCount));
        }

        m_impl->statusSummary = QStringLiteral("Loaded %1 mailboxes and %2 messages from %3.")
                                    .arg(parsedMailboxes.list.size())
                                    .arg(emailCount)
                                    .arg(QString::fromStdString(settings.loginEmail));
        qInfo().noquote() << "JMAP core refresh success" << m_impl->statusSummary;

        const auto pendingSubmit = co_await submitPendingEmailMutations(settings, accountId);
        if (const auto* summary = std::get_if<SubmittedEmailMutations>(&pendingSubmit);
            summary != nullptr && summary->updatedEmailCount > 0)
        {
            reportProgress(QStringLiteral("Submitted %1 queued mailbox updates.")
                               .arg(summary->updatedEmailCount));
        }

        co_return LiveRefreshSummary{
            .accountId = accountId,
            .selectedMailboxId = selectedMailboxId,
            .mailboxCount = parsedMailboxes.list.size(),
            .emailCount = emailCount,
            .resolvedSessionUrl = sessionClient.resolvedSessionUrl(),
        };
    }

    QCoro::Task<MessageContentRefreshResult>
    JmapCore::refreshMessageContent(LiveConnectionSettings settings, std::string accountId,
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

        qInfo().noquote() << "JMAP core message content refresh start"
                          << QString::fromStdString(accountId) << QString::fromStdString(emailId);
        reportProgress(QStringLiteral("Checking for saved message content..."));
        if (m_impl->databaseConnection == nullptr || m_impl->transport == nullptr)
        {
            co_return LiveRefreshError{
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
        if (const auto* error = std::get_if<LiveRefreshError>(&emailResult))
        {
            co_return *error;
        }
        const auto& email = std::get<javelin::jmap::domain::Email>(emailResult);

        const auto cachedSource = sourceRepository.find(accountId, emailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&cachedSource))
        {
            co_return LiveRefreshError{.message = error->message};
        }
        const auto& source =
            std::get<std::optional<javelin::jmap::cache::RawMessageSource>>(cachedSource);
        if (source.has_value() && source->blobId == email.blobId)
        {
            qInfo().noquote() << "JMAP core message source using cached data"
                              << QString::fromStdString(emailId);
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
        if (const auto* error = std::get_if<LiveRefreshError>(&sessionResult))
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
        const auto downloadResult = co_await downloadBlob(
            *m_impl->transport, context.session.downloadUrl, accountId, sourcePart,
            context.accessToken, QStringLiteral("Message source download"));
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

        const auto payload = std::get<QByteArray>(downloadResult);
        if (const auto error = sourceRepository.upsert(accountId, {
                                                                      .emailId = email.id,
                                                                      .blobId = email.blobId,
                                                                      .payload = payload,
                                                                  }))
        {
            co_return LiveRefreshError{.message = error->message};
        }

        qInfo().noquote() << "JMAP core message source refresh success"
                          << QString::fromStdString(emailId)
                          << static_cast<qulonglong>(payload.size());
        reportProgress(QStringLiteral("Message ready."));

        co_return MessageContentRefreshSummary{
            .accountId = std::move(accountId),
            .emailId = std::move(emailId),
            .partCount = 1,
            .bodyValueCount = 0,
            .usedCachedContent = false,
        };
    }

    QCoro::Task<AttachmentDownloadResult>
    JmapCore::downloadAttachment(LiveConnectionSettings settings, std::string accountId,
                                 std::string emailId, std::string partId)
    {
        Q_UNUSED(settings);

        if (m_impl->databaseConnection == nullptr)
        {
            co_return LiveRefreshError{
                .message = QStringLiteral(
                    "Attachment download is unavailable in this process configuration."),
            };
        }

        const auto emailResult =
            findEmailForDownload(*m_impl->databaseConnection, accountId, emailId);
        if (const auto* error = std::get_if<LiveRefreshError>(&emailResult))
        {
            co_return *error;
        }
        const auto& email = std::get<javelin::jmap::domain::Email>(emailResult);

        javelin::jmap::cache::RawMessageSourceRepository sourceRepository{
            *m_impl->databaseConnection};
        const auto sourceResult = sourceRepository.find(accountId, emailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&sourceResult))
        {
            co_return LiveRefreshError{.message = error->message};
        }

        const auto& source =
            std::get<std::optional<javelin::jmap::cache::RawMessageSource>>(sourceResult);
        if (!source.has_value() || source->blobId != email.blobId)
        {
            co_return LiveRefreshError{
                .message = QStringLiteral(
                    "The selected attachment is not cached locally. Open the message first."),
            };
        }

        const auto parsedPart =
            javelin::jmap::cache::findMessageSourcePart(emailId, source->payload, partId);
        if (!parsedPart.has_value())
        {
            co_return LiveRefreshError{
                .message =
                    QStringLiteral("The selected attachment is not present in the cached message."),
            };
        }

        const auto& part = parsedPart->part;
        const bool isAttachment = part.kind == "attachment" || part.name.has_value() ||
                                  part.disposition.has_value() || part.cid.has_value();
        if (!isAttachment)
        {
            co_return LiveRefreshError{
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
    JmapCore::downloadMessageSource(LiveConnectionSettings settings, std::string accountId,
                                    std::string emailId)
    {
        if (m_impl->databaseConnection == nullptr || m_impl->transport == nullptr)
        {
            co_return LiveRefreshError{
                .message = QStringLiteral(
                    "Message source download is unavailable in this process configuration."),
            };
        }

        if (const auto validationError = validateLoginSettings(settings, true))
        {
            co_return *validationError;
        }

        const auto emailResult =
            findEmailForDownload(*m_impl->databaseConnection, accountId, emailId);
        if (const auto* error = std::get_if<LiveRefreshError>(&emailResult))
        {
            co_return *error;
        }

        const auto& email = std::get<javelin::jmap::domain::Email>(emailResult);

        const auto downloadContextResult =
            prepareDownloadContext(*m_impl->databaseConnection, settings, accountId);
        if (const auto* error = std::get_if<LiveRefreshError>(&downloadContextResult))
        {
            co_return *error;
        }
        const auto& downloadContext = std::get<DownloadContext>(downloadContextResult);
        const javelin::jmap::cache::EmailPart messageBlob{
            .emailId = email.id,
            .partId = email.id,
            .parentPartId = std::nullopt,
            .blobId = email.blobId,
            .kind = "message",
            .mediaType = "message/rfc822",
            .name = std::nullopt,
            .charset = std::nullopt,
            .disposition = std::nullopt,
            .cid = std::nullopt,
            .size = email.size,
            .isInlineRenderable = false,
            .isBodySection = false,
        };
        const auto payloadResult = co_await downloadBlob(
            *m_impl->transport, downloadContext.session.downloadUrl, accountId, messageBlob,
            downloadContext.accessToken, QStringLiteral("Message source download"));
        if (const auto* error = std::get_if<BlobDownloadError>(&payloadResult))
        {
            co_return error->error;
        }

        co_return MessageSourceDownload{
            .accountId = std::move(accountId),
            .emailId = std::move(emailId),
            .blobId = email.blobId,
            .subject = email.subject,
            .payload = std::get<QByteArray>(payloadResult),
        };
    }

    QueuedEmailMutationResult JmapCore::queueMoveEmail(std::string accountId, std::string emailId,
                                                       std::string sourceMailboxId,
                                                       std::string destinationMailboxId)
    {
        if (m_impl->databaseConnection == nullptr)
        {
            return LiveRefreshError{
                .message = QStringLiteral("Queued mutations are unavailable in this process."),
            };
        }

        return queueMailboxPatch(*m_impl->databaseConnection, std::move(accountId),
                                 std::move(emailId), std::move(sourceMailboxId),
                                 std::move(destinationMailboxId), true);
    }

    QueuedEmailMutationResult JmapCore::queueCopyEmail(std::string accountId, std::string emailId,
                                                       std::string sourceMailboxId,
                                                       std::string destinationMailboxId)
    {
        if (m_impl->databaseConnection == nullptr)
        {
            return LiveRefreshError{
                .message = QStringLiteral("Queued mutations are unavailable in this process."),
            };
        }

        return queueMailboxPatch(*m_impl->databaseConnection, std::move(accountId),
                                 std::move(emailId), std::move(sourceMailboxId),
                                 std::move(destinationMailboxId), false);
    }

    QueuedEmailMutationResult JmapCore::queueArchiveEmail(std::string accountId,
                                                          std::string emailId,
                                                          std::string sourceMailboxId,
                                                          std::string archiveMailboxId)
    {
        return queueMoveEmail(std::move(accountId), std::move(emailId), std::move(sourceMailboxId),
                              std::move(archiveMailboxId));
    }

    QueuedEmailMutationResult JmapCore::queueDeleteEmail(std::string accountId, std::string emailId,
                                                         std::string sourceMailboxId,
                                                         std::string trashMailboxId)
    {
        return queueMoveEmail(std::move(accountId), std::move(emailId), std::move(sourceMailboxId),
                              std::move(trashMailboxId));
    }

    QueuedEmailMutationResult JmapCore::queueDestroyEmail(std::string accountId,
                                                          std::string emailId)
    {
        if (m_impl->databaseConnection == nullptr)
        {
            return LiveRefreshError{
                .message = QStringLiteral("Queued mutations are unavailable in this process."),
            };
        }

        return queueDestroyEmailMutation(*m_impl->databaseConnection, std::move(accountId),
                                         std::move(emailId));
    }

    QueuedEmailMutationResult JmapCore::queueMarkEmailRead(std::string accountId,
                                                           std::string emailId)
    {
        if (m_impl->databaseConnection == nullptr)
        {
            return LiveRefreshError{
                .message = QStringLiteral("Queued mutations are unavailable in this process."),
            };
        }

        return queueKeywordPatch(*m_impl->databaseConnection, std::move(accountId),
                                 std::move(emailId), "$seen", true);
    }

    QueuedEmailMutationResult JmapCore::queueMarkEmailUnread(std::string accountId,
                                                             std::string emailId)
    {
        if (m_impl->databaseConnection == nullptr)
        {
            return LiveRefreshError{
                .message = QStringLiteral("Queued mutations are unavailable in this process."),
            };
        }

        return queueKeywordPatch(*m_impl->databaseConnection, std::move(accountId),
                                 std::move(emailId), "$seen", false);
    }

    QueuedEmailMutationResult
    JmapCore::queueSetEmailFlagged(std::string accountId, std::string emailId, const bool flagged)
    {
        if (m_impl->databaseConnection == nullptr)
        {
            return LiveRefreshError{
                .message = QStringLiteral("Queued mutations are unavailable in this process."),
            };
        }

        return queueKeywordPatch(*m_impl->databaseConnection, std::move(accountId),
                                 std::move(emailId), "$flagged", flagged);
    }

    QCoro::Task<SubmittedEmailMutationsResult>
    JmapCore::submitPendingEmailMutations(LiveConnectionSettings settings, std::string accountId,
                                          const std::size_t limit)
    {
        if (m_impl->databaseConnection == nullptr || m_impl->transport == nullptr)
        {
            co_return LiveRefreshError{
                .message = QStringLiteral("JMAP core is not wired to the cache and transport yet."),
            };
        }

        if (const auto validationError = validateLoginSettings(settings, true))
        {
            co_return *validationError;
        }

        const auto sessionResult = loadCachedSession(*m_impl->databaseConnection, accountId);
        if (const auto* error = std::get_if<LiveRefreshError>(&sessionResult))
        {
            co_return *error;
        }
        const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);

        javelin::jmap::sync::PendingActionRepository pendingActionRepository{
            *m_impl->databaseConnection};
        const auto pendingResult = pendingActionRepository.listByStatus(
            accountId, javelin::jmap::sync::PendingActionStatus::Pending, limit);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&pendingResult))
        {
            co_return LiveRefreshError{.message = error->message};
        }

        const auto& pendingActions =
            std::get<std::vector<javelin::jmap::sync::PendingActionRecord>>(pendingResult);
        if (pendingActions.empty())
        {
            co_return SubmittedEmailMutations{
                .accountId = std::move(accountId),
                .attemptedEmailCount = 0,
                .updatedEmailCount = 0,
                .failedEmailCount = 0,
            };
        }

        std::vector<std::string> emailIds;
        std::unordered_set<std::string> seenEmailIds;
        for (const auto& action : pendingActions)
        {
            if (seenEmailIds.insert(action.emailPatch.emailId).second)
            {
                emailIds.push_back(action.emailPatch.emailId);
            }
        }

        javelin::jmap::cache::EmailRepository emailRepository{*m_impl->databaseConnection};
        std::unordered_map<std::string, javelin::jmap::domain::Email> mergedEmails;
        std::unordered_map<std::string, std::vector<std::string>> pendingActionIdsByEmailId;
        for (const auto& emailId : emailIds)
        {
            const auto emailResult = emailRepository.find(accountId, emailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&emailResult))
            {
                co_return LiveRefreshError{.message = error->message};
            }

            const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(emailResult);
            if (!email.has_value())
            {
                co_return LiveRefreshError{
                    .message = QStringLiteral("A queued email mutation targets a missing email."),
                };
            }

            const auto allEmailActionsResult =
                pendingActionRepository.listForEmail(accountId, emailId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&allEmailActionsResult))
            {
                co_return LiveRefreshError{.message = error->message};
            }

            const auto allEmailActions = activePendingActions(
                std::get<std::vector<javelin::jmap::sync::PendingActionRecord>>(
                    allEmailActionsResult));
            if (allEmailActions.empty())
            {
                continue;
            }

            mergedEmails.emplace(
                emailId, javelin::jmap::sync::mergePendingEmailPatch(*email, allEmailActions));

            auto& pendingIds = pendingActionIdsByEmailId[emailId];
            pendingIds.reserve(allEmailActions.size());
            for (const auto& action : allEmailActions)
            {
                pendingIds.push_back(action.pendingActionId);
                if (const auto error = pendingActionRepository.updateStatus(
                        action.pendingActionId, javelin::jmap::sync::PendingActionStatus::InFlight))
                {
                    co_return LiveRefreshError{.message = error->message};
                }
            }
        }

        std::unordered_map<std::string, javelin::jmap::api::EmailSetUpdate> updates;
        std::vector<std::string> destroys;
        updates.reserve(mergedEmails.size());
        destroys.reserve(mergedEmails.size());
        for (const auto& [emailId, email] : mergedEmails)
        {
            const auto actionsResult = pendingActionRepository.listForEmail(accountId, emailId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&actionsResult))
            {
                co_return LiveRefreshError{.message = error->message};
            }
            const auto actions = activePendingActions(
                std::get<std::vector<javelin::jmap::sync::PendingActionRecord>>(actionsResult));
            const bool destroy = std::ranges::any_of(actions, [](const auto& action)
                                                     { return action.emailPatch.destroy; });
            if (destroy)
            {
                destroys.push_back(emailId);
                continue;
            }

            updates.emplace(emailId, javelin::jmap::api::EmailSetUpdate{
                                         .mailboxIds = enabledMap(email.mailboxIds),
                                         .keywords = enabledMap(email.keywords),
                                     });
        }

        const auto requestMethod = javelin::jmap::api::emailSet({
            .accountId = accountId,
            .create = {},
            .update = std::move(updates),
            .destroy = std::move(destroys),
        });
        if (!requestMethod.has_value())
        {
            co_return LiveRefreshError{
                .message = QStringLiteral("Failed to encode the Email/set request."),
            };
        }

        javelin::jmap::api::MethodCaller methodCaller{*m_impl->transport};
        const auto apiRequestContext = buildApiRequestContext(settings, accountId, session);
        javelin::jmap::api::RequestBuilder requestBuilder;
        requestBuilder.useCore().useMail();
        const auto setHandle = requestBuilder.call(*requestMethod, "queued-email-set");

        const auto envelopeResult = co_await methodCaller.call(apiRequestContext, requestBuilder);
        if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&envelopeResult))
        {
            co_return LiveRefreshError{.message = transportMessage(*error)};
        }
        if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&envelopeResult))
        {
            co_return LiveRefreshError{
                .message = authMessage(*error),
                .requiresUserIntervention = true,
            };
        }
        if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&envelopeResult))
        {
            co_return LiveRefreshError{.message = protocolMessage(*error)};
        }

        const auto& envelope = std::get<javelin::jmap::api::ResponseEnvelope>(envelopeResult);
        const javelin::jmap::api::ResponseReader reader{envelope};
        const auto parsedResult = reader.require(setHandle);
        if (const auto* error = std::get_if<javelin::jmap::api::ResponseReaderError>(&parsedResult))
        {
            co_return LiveRefreshError{
                .message = QStringLiteral("Failed to read Email/set response: %1")
                               .arg(QString::fromStdString(error->message)),
            };
        }
        const auto& parsed = std::get<javelin::jmap::api::EmailSetResponse>(parsedResult);

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

        for (const auto& [emailId, email] : mergedEmails)
        {
            const auto idsIt = pendingActionIdsByEmailId.find(emailId);
            if (idsIt == pendingActionIdsByEmailId.end())
            {
                continue;
            }

            if (updatedEmailIds.contains(emailId) || destroyedEmailIds.contains(emailId))
            {
                if (destroyedEmailIds.contains(emailId))
                {
                    const std::array destroyed{emailId};
                    if (const auto error = emailRepository.removeMany(accountId, destroyed))
                    {
                        co_return LiveRefreshError{.message = error->message};
                    }
                }
                else if (const auto error = emailRepository.upsertMany(accountId, {email}))
                {
                    co_return LiveRefreshError{.message = error->message};
                }

                for (const auto& pendingActionId : idsIt->second)
                {
                    if (const auto error = pendingActionRepository.remove(pendingActionId))
                    {
                        co_return LiveRefreshError{.message = error->message};
                    }
                }
                continue;
            }

            for (const auto& pendingActionId : idsIt->second)
            {
                const auto status = failedEmailIds.contains(emailId)
                                        ? javelin::jmap::sync::PendingActionStatus::Failed
                                        : javelin::jmap::sync::PendingActionStatus::Pending;
                if (const auto error =
                        pendingActionRepository.updateStatus(pendingActionId, status))
                {
                    co_return LiveRefreshError{.message = error->message};
                }
            }
        }

        co_return SubmittedEmailMutations{
            .accountId = std::move(accountId),
            .attemptedEmailCount = mergedEmails.size(),
            .updatedEmailCount = updatedEmailIds.size() + destroyedEmailIds.size(),
            .failedEmailCount = failedEmailIds.size(),
        };
    }

    QCoro::Task<MailboxMessagesRefreshResult>
    JmapCore::refreshMailboxMessages(LiveConnectionSettings settings, std::string accountId,
                                     std::string mailboxId,
                                     std::function<void(const QString&)> progressCallback)
    {
        const auto reportProgress = [&progressCallback](const QString& message)
        {
            if (progressCallback)
            {
                progressCallback(message);
            }
        };

        qInfo().noquote() << "JMAP core mailbox refresh start" << QString::fromStdString(accountId)
                          << QString::fromStdString(mailboxId);
        reportProgress(QStringLiteral("Fetching messages for selected mailbox..."));
        if (m_impl->databaseConnection == nullptr || m_impl->transport == nullptr)
        {
            co_return LiveRefreshError{
                .message = QStringLiteral("JMAP core is not wired to the cache and transport yet."),
            };
        }

        if (const auto validationError = validateLoginSettings(settings, true))
        {
            co_return *validationError;
        }

        const auto sessionResult = loadCachedSession(*m_impl->databaseConnection, accountId);
        if (const auto* error = std::get_if<LiveRefreshError>(&sessionResult))
        {
            co_return *error;
        }
        const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);

        javelin::jmap::api::MethodCaller methodCaller{*m_impl->transport};
        const auto apiRequestContext = buildApiRequestContext(settings, accountId, session);

        javelin::jmap::sync::MailboxRefreshExecutor mailboxRefreshExecutor{
            *m_impl->databaseConnection, methodCaller, apiRequestContext};
        const auto refreshResult = co_await mailboxRefreshExecutor.refreshCollapsedMailbox(
            accountId, mailboxId, reportProgress, true);
        if (const auto* error =
                std::get_if<javelin::jmap::sync::MailboxRefreshError>(&refreshResult))
        {
            co_return LiveRefreshError{.message = error->message};
        }

        const auto representativeCount =
            std::get<javelin::jmap::sync::MailboxRefreshSummary>(refreshResult).representativeCount;

        const auto pendingSubmit = co_await submitPendingEmailMutations(settings, accountId);
        if (const auto* summary = std::get_if<SubmittedEmailMutations>(&pendingSubmit);
            summary != nullptr && summary->updatedEmailCount > 0)
        {
            reportProgress(QStringLiteral("Submitted %1 queued mailbox updates.")
                               .arg(summary->updatedEmailCount));
        }

        co_return MailboxMessagesRefreshSummary{
            .accountId = std::move(accountId),
            .mailboxId = std::move(mailboxId),
            .emailCount = representativeCount,
        };
    }

    QCoro::Task<MessageSearchResult>
    JmapCore::searchMessages(LiveConnectionSettings settings, std::string accountId,
                             std::string query, const std::size_t offset, const std::size_t limit,
                             javelin::jmap::query::EmailListSort sort,
                             std::function<void(const QString&)> progressCallback)
    {
        co_return co_await searchMessages(
            std::move(settings), std::move(accountId),
            javelin::jmap::search::EmailSearchCriteria{.text = std::move(query)}, offset, limit,
            std::move(sort), std::move(progressCallback));
    }

    QCoro::Task<MessageSearchResult>
    JmapCore::searchMessages(LiveConnectionSettings settings, std::string accountId,
                             javelin::jmap::search::EmailSearchCriteria criteria,
                             const std::size_t offset, const std::size_t limit,
                             javelin::jmap::query::EmailListSort sort,
                             std::function<void(const QString&)> progressCallback)
    {
        const auto reportProgress = [&progressCallback](const QString& message)
        {
            if (progressCallback)
            {
                progressCallback(message);
            }
        };

        auto query = javelin::jmap::search::displayString(criteria);
        qInfo().noquote() << "JMAP core search start" << QString::fromStdString(accountId)
                          << QString::fromStdString(query);
        reportProgress(QStringLiteral("Searching the server..."));
        if (m_impl->databaseConnection == nullptr || m_impl->transport == nullptr)
        {
            co_return LiveRefreshError{
                .message = QStringLiteral("JMAP core is not wired to the cache and transport yet."),
            };
        }

        if (javelin::jmap::search::isEmpty(criteria))
        {
            co_return LiveRefreshError{
                .message = QStringLiteral("Enter a search term before searching."),
            };
        }

        const auto pageResult = co_await performCollapsedQueryPage(
            *m_impl->databaseConnection, *m_impl->transport, settings, accountId,
            javelin::jmap::search::toEmailQueryFilter(criteria), offset, limit, std::move(sort),
            reportProgress);
        if (const auto* error = std::get_if<LiveRefreshError>(&pageResult))
        {
            co_return *error;
        }

        auto page = std::get<CollapsedQueryPage>(std::move(pageResult));
        co_return MessageSearchSummary{
            .accountId = std::move(accountId),
            .query = std::move(query),
            .offset = offset,
            .limit = limit,
            .representativeCount = page.representativeCount,
            .total = page.total,
            .results = std::move(page.results),
        };
    }

    QCoro::Task<MailboxPageResult>
    JmapCore::queryMailboxPage(LiveConnectionSettings settings, std::string accountId,
                               std::string mailboxId, const std::size_t offset,
                               const std::size_t limit, javelin::jmap::query::EmailListSort sort,
                               std::function<void(const QString&)> progressCallback)
    {
        const auto reportProgress = [&progressCallback](const QString& message)
        {
            if (progressCallback)
            {
                progressCallback(message);
            }
        };

        qInfo().noquote() << "JMAP core mailbox page query" << QString::fromStdString(accountId)
                          << QString::fromStdString(mailboxId) << static_cast<qulonglong>(offset)
                          << static_cast<qulonglong>(limit);
        reportProgress(QStringLiteral("Fetching mailbox page from the server..."));
        if (m_impl->databaseConnection == nullptr || m_impl->transport == nullptr)
        {
            co_return LiveRefreshError{
                .message = QStringLiteral("JMAP core is not wired to the cache and transport yet."),
            };
        }

        if (const auto validationError = validateLoginSettings(settings, true))
        {
            co_return *validationError;
        }
        const auto sessionResult = loadCachedSession(*m_impl->databaseConnection, accountId);
        if (const auto* error = std::get_if<LiveRefreshError>(&sessionResult))
        {
            co_return *error;
        }
        javelin::jmap::api::MethodCaller methodCaller{*m_impl->transport};
        javelin::jmap::sync::MailboxRefreshExecutor mailboxRefreshExecutor{
            *m_impl->databaseConnection, methodCaller,
            buildApiRequestContext(settings, accountId,
                                   std::get<javelin::jmap::api::Session>(sessionResult))};
        const auto refreshResult = co_await mailboxRefreshExecutor.refreshCollapsedMailbox(
            accountId, mailboxId, reportProgress, true);
        if (const auto* error =
                std::get_if<javelin::jmap::sync::MailboxRefreshError>(&refreshResult))
        {
            co_return LiveRefreshError{.message = error->message};
        }

        const auto pageResult = co_await performCollapsedQueryPage(
            *m_impl->databaseConnection, *m_impl->transport, settings, accountId,
            javelin::jmap::api::EmailQueryFilter{
                .inMailbox = mailboxId,
                .text = std::nullopt,
            },
            offset, limit, std::move(sort), reportProgress);
        if (const auto* error = std::get_if<LiveRefreshError>(&pageResult))
        {
            co_return *error;
        }

        auto page = std::get<CollapsedQueryPage>(std::move(pageResult));
        co_return MailboxPageSummary{
            .accountId = std::move(accountId),
            .mailboxId = std::move(mailboxId),
            .offset = offset,
            .limit = limit,
            .representativeCount = page.representativeCount,
            .total = page.total,
            .results = std::move(page.results),
        };
    }

} // namespace javelin::jmap
