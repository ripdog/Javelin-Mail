#include "jmap/JmapCore.h"

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
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/MessageContentTypes.h"
#include "jmap/cache/MimeMessageParser.h"
#include "jmap/cache/RawMessageSourceRepository.h"
#include "jmap/cache/SearchWindowRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/cache/ThreadRepository.h"
#include "jmap/sync/ConsistencyDomain.h"
#include "jmap/sync/EmailMutationJournal.h"
#include "jmap/sync/MailboxQueryDescriptor.h"
#include "jmap/sync/MailboxRefreshExecutor.h"

#include <QCoroFuture>

#include <QDebug>
#include <QSqlError>
#include <QSqlQuery>
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
    struct JmapCore::Impl
    {
        javelin::jmap::cache::DatabaseConnection* databaseConnection = nullptr;
        javelin::jmap::api::AbstractTransport* resourceTransport = nullptr;
        javelin::jmap::api::JmapMethodTransport* methodTransport = nullptr;
    };

    namespace
    {

        struct DownloadContext
        {
            javelin::jmap::auth::AccountCredentials credentials;
            javelin::jmap::api::Session session;
            std::string accessToken;
        };

        struct BlobDownloadError
        {
            OperationError error;
            std::optional<int> httpStatus;
        };

        [[nodiscard]] QString storeRawMessageSource(const QString& databasePath,
                                                    const std::string& accountId,
                                                    const std::string& emailId,
                                                    const std::string& blobId, QByteArray payload)
        {
            javelin::jmap::cache::ThreadConnectionFactory factory({
                .connectionNamePrefix = QStringLiteral("message-source-store"),
                .databasePath = databasePath,
            });
            auto opened = factory.openForCurrentThread(accountId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
                return error->message;
            auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
            javelin::jmap::cache::RawMessageSourceRepository sources{connection};
            if (const auto error = sources.upsert(accountId, {
                                                                 .emailId = emailId,
                                                                 .blobId = blobId,
                                                                 .payload = std::move(payload),
                                                             }))
                return error->message;
            return {};
        }

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

            auto credentials = buildAccountCredentials(settings, accountId);
            const auto sessionResult = loadCachedSession(connection, accountId);
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
                    .error = operationError(*error),
                    .httpStatus = error->httpStatus,
                };
            }

            const auto& response = std::get<javelin::jmap::api::HttpResponse>(transportResult);
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

            co_return response.body;
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

        struct EmailMutationBase
        {
            std::vector<std::string> mailboxIds;
            std::vector<std::string> keywords;
        };

        [[nodiscard]] std::variant<EmailMutationBase, javelin::jmap::cache::DatabaseError>
        emailMutationBase(javelin::jmap::sync::EmailMutationJournal& journal,
                          const std::string_view accountId,
                          const javelin::jmap::domain::Email& email)
        {
            const auto recordsResult = journal.listForEmail(accountId, email.id);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&recordsResult))
            {
                return *error;
            }
            for (const auto& record :
                 std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(recordsResult))
            {
                if (javelin::jmap::sync::projectsOptimistically(record.status) &&
                    record.baseMailboxIds.has_value() && record.baseKeywords.has_value())
                {
                    return EmailMutationBase{
                        .mailboxIds = *record.baseMailboxIds,
                        .keywords = *record.baseKeywords,
                    };
                }
            }
            return EmailMutationBase{
                .mailboxIds = email.mailboxIds,
                .keywords = email.keywords,
            };
        }

        [[nodiscard]] QueuedEmailMutationResult
        queueEmailPatch(javelin::jmap::cache::DatabaseConnection& connection, std::string accountId,
                        EmailMailboxMutation mutation)
        {
            if (mutation.emailId.empty())
            {
                return OperationError{
                    .message = QStringLiteral("An email id is required for an email patch."),
                };
            }
            if (mutation.addMailboxIds.empty() && mutation.removeMailboxIds.empty() &&
                mutation.addKeywords.empty() && mutation.removeKeywords.empty())
            {
                return OperationError{
                    .message = QStringLiteral("An email mutation must change a property."),
                };
            }
            for (const auto& mailboxId : mutation.addMailboxIds)
            {
                if (mailboxId.empty() || std::ranges::find(mutation.removeMailboxIds, mailboxId) !=
                                             mutation.removeMailboxIds.end())
                {
                    return OperationError{
                        .message = QStringLiteral(
                            "Email mailbox additions and removals must be non-empty and disjoint."),
                    };
                }
            }
            if (std::ranges::any_of(mutation.removeMailboxIds,
                                    [](const std::string& mailboxId) { return mailboxId.empty(); }))
            {
                return OperationError{
                    .message = QStringLiteral("Email mailbox removals must be non-empty."),
                };
            }
            for (const auto& keyword : mutation.addKeywords)
            {
                if (keyword.empty() || std::ranges::find(mutation.removeKeywords, keyword) !=
                                           mutation.removeKeywords.end())
                {
                    return OperationError{
                        .message = QStringLiteral(
                            "Email keyword additions and removals must be non-empty and disjoint."),
                    };
                }
            }
            if (std::ranges::any_of(mutation.removeKeywords,
                                    [](const std::string& keyword) { return keyword.empty(); }))
            {
                return OperationError{
                    .message = QStringLiteral("Email keyword removals must be non-empty."),
                };
            }

            javelin::jmap::cache::EmailRepository emailRepository{connection};
            const auto emailResult = emailRepository.find(accountId, mutation.emailId);
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

            if (mutation.authoritativeMailboxIds.has_value() !=
                mutation.authoritativeKeywords.has_value())
            {
                return OperationError{
                    .message = QStringLiteral("An authoritative email mutation base must include "
                                              "mailboxes and keywords."),
                };
            }

            auto effectiveEmail = *email;
            if (mutation.authoritativeMailboxIds.has_value())
            {
                effectiveEmail.mailboxIds = *mutation.authoritativeMailboxIds;
                effectiveEmail.keywords = *mutation.authoritativeKeywords;
            }

            auto resultingMailboxIds = effectiveEmail.mailboxIds;
            std::erase_if(resultingMailboxIds,
                          [&mutation](const std::string& mailboxId)
                          {
                              return std::ranges::find(mutation.removeMailboxIds, mailboxId) !=
                                     mutation.removeMailboxIds.end();
                          });
            resultingMailboxIds.insert(resultingMailboxIds.end(), mutation.addMailboxIds.begin(),
                                       mutation.addMailboxIds.end());
            if (resultingMailboxIds.empty())
            {
                return OperationError{
                    .message = QStringLiteral("An email must remain in at least one mailbox."),
                };
            }

            javelin::jmap::sync::EmailMutationJournal emailMutationJournal{connection};
            const auto baseResult =
                mutation.authoritativeMailboxIds.has_value()
                    ? std::variant<EmailMutationBase,
                                   javelin::jmap::cache::DatabaseError>{EmailMutationBase{
                          .mailboxIds = *mutation.authoritativeMailboxIds,
                          .keywords = *mutation.authoritativeKeywords,
                      }}
                    : emailMutationBase(emailMutationJournal, accountId, effectiveEmail);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&baseResult))
            {
                return javelin::jmap::operationError(*error);
            }
            const auto& base = std::get<EmailMutationBase>(baseResult);
            const auto mutationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const javelin::jmap::sync::EmailMutationRecord pendingAction{
                .mutationId = mutationId.toStdString(),
                .operationGroupId = mutation.operationGroupId,
                .accountId = accountId,
                .status = javelin::jmap::sync::MutationStatus::Pending,
                .patch =
                    {
                        .emailId = mutation.emailId,
                        .addMailboxIds = mutation.addMailboxIds,
                        .removeMailboxIds = mutation.removeMailboxIds,
                        .addKeywords = mutation.addKeywords,
                        .removeKeywords = mutation.removeKeywords,
                    },
                .baseMailboxIds = base.mailboxIds,
                .baseKeywords = base.keywords,
                .baseState = mutation.ifInState,
                .acceptedState = std::nullopt,
                .errorJson = std::nullopt,
            };

            const auto reconciledEmail =
                javelin::jmap::sync::projectEmailMutations(effectiveEmail, {pendingAction});
            if (const auto error = emailMutationJournal.queue(pendingAction, reconciledEmail))
            {
                return javelin::jmap::operationError(*error);
            }
            javelin::jmap::cache::RawMessageSourceRepository sources{connection};
            if (const auto projectionError = sources.replayProjectionJobs())
            {
                qWarning().noquote()
                    << "Mail vault mailbox projection deferred" << projectionError->message;
            }

            return QueuedEmailMutation{
                .mutationId = mutationId.toStdString(),
                .accountId = accountId,
                .emailId = mutation.emailId,
                .patch = std::move(mutation),
            };
        }

        [[nodiscard]] QueuedEmailMutationResult
        queueMailboxPatch(javelin::jmap::cache::DatabaseConnection& connection,
                          std::string accountId, std::string emailId, std::string sourceMailboxId,
                          std::string destinationMailboxId, const bool removeSourceMailbox)
        {
            if (sourceMailboxId.empty() || destinationMailboxId.empty())
            {
                return OperationError{
                    .message = QStringLiteral("Source and destination mailbox ids are required."),
                };
            }
            if (sourceMailboxId == destinationMailboxId)
            {
                return OperationError{
                    .message =
                        QStringLiteral("Source and destination mailboxes must be different."),
                };
            }

            return queueEmailPatch(
                connection, std::move(accountId),
                EmailMailboxMutation{
                    .emailId = std::move(emailId),
                    .addMailboxIds = {std::move(destinationMailboxId)},
                    .removeMailboxIds = removeSourceMailbox
                                            ? std::vector<std::string>{std::move(sourceMailboxId)}
                                            : std::vector<std::string>{},
                    .addKeywords = {},
                    .removeKeywords = {},
                    .operationGroupId = std::nullopt,
                    .ifInState = std::nullopt,
                    .authoritativeMailboxIds = std::nullopt,
                    .authoritativeKeywords = std::nullopt,
                });
        }

        [[nodiscard]] QueuedEmailMutationResult
        queueDestroyEmailMutation(javelin::jmap::cache::DatabaseConnection& connection,
                                  std::string accountId, std::string emailId,
                                  std::optional<std::string> operationGroupId)
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

            javelin::jmap::sync::EmailMutationJournal emailMutationJournal{connection};
            const auto baseResult = emailMutationBase(emailMutationJournal, accountId, *email);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&baseResult))
            {
                return javelin::jmap::operationError(*error);
            }
            const auto& base = std::get<EmailMutationBase>(baseResult);
            const auto mutationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const javelin::jmap::sync::EmailMutationRecord pendingAction{
                .mutationId = mutationId.toStdString(),
                .operationGroupId = operationGroupId,
                .accountId = accountId,
                .status = javelin::jmap::sync::MutationStatus::Pending,
                .patch =
                    {
                        .emailId = emailId,
                        .addMailboxIds = {},
                        .removeMailboxIds = email->mailboxIds,
                        .addKeywords = {},
                        .removeKeywords = {},
                        .destroy = true,
                    },
                .baseMailboxIds = base.mailboxIds,
                .baseKeywords = base.keywords,
                .baseState = std::nullopt,
                .acceptedState = std::nullopt,
                .errorJson = std::nullopt,
            };

            const auto reconciledEmail =
                javelin::jmap::sync::projectEmailMutations(*email, {pendingAction});
            if (const auto error = emailMutationJournal.queue(pendingAction, reconciledEmail))
            {
                return javelin::jmap::operationError(*error);
            }

            return QueuedEmailMutation{
                .mutationId = mutationId.toStdString(),
                .accountId = std::move(accountId),
                .emailId = std::move(emailId),
                .patch =
                    {
                        .emailId = pendingAction.patch.emailId,
                        .addMailboxIds = {},
                        .removeMailboxIds = pendingAction.patch.removeMailboxIds,
                        .addKeywords = {},
                        .removeKeywords = {},
                        .operationGroupId = std::move(operationGroupId),
                        .ifInState = std::nullopt,
                        .authoritativeMailboxIds = std::nullopt,
                        .authoritativeKeywords = std::nullopt,
                    },
            };
        }

        [[nodiscard]] QueuedEmailMutationResult
        queueKeywordPatch(javelin::jmap::cache::DatabaseConnection& connection,
                          std::string accountId, std::string emailId, std::string keyword,
                          const bool enabled)
        {
            return queueEmailPatch(connection, std::move(accountId),
                                   EmailMailboxMutation{
                                       .emailId = std::move(emailId),
                                       .addMailboxIds = {},
                                       .removeMailboxIds = {},
                                       .addKeywords = enabled ? std::vector<std::string>{keyword}
                                                              : std::vector<std::string>{},
                                       .removeKeywords = enabled
                                                             ? std::vector<std::string>{}
                                                             : std::vector<std::string>{keyword},
                                       .operationGroupId = std::nullopt,
                                       .ifInState = std::nullopt,
                                       .authoritativeMailboxIds = std::nullopt,
                                       .authoritativeKeywords = std::nullopt,
                                   });
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
                .mailboxNames = {},
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
            std::size_t position = 0;
            std::size_t returnedLimit = 0;
            std::optional<std::size_t> total;
            std::string queryState;
            std::vector<javelin::jmap::cache::MessageListItem> results;
        };

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

            const auto sessionResult = loadCachedSession(databaseConnection, accountId);
            if (const auto* error = std::get_if<OperationError>(&sessionResult))
            {
                co_return *error;
            }
            const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);

            javelin::jmap::api::MethodCaller methodCaller{methodTransport};
            const auto apiRequestContext = buildApiRequestContext(settings, accountId, session);

            javelin::jmap::api::RequestBuilder builder;
            builder.useCore().useMail();

            const auto queryRequest = javelin::jmap::api::emailQuery({
                .accountId = accountId,
                .filter = filter,
                .sort = {javelin::jmap::query::toEmailQuerySort(sort)},
                .position = anchor.has_value()
                                ? std::nullopt
                                : std::optional<std::uint64_t>{static_cast<std::uint64_t>(offset)},
                .anchor = std::move(anchor),
                .anchorOffset = anchorOffset,
                .limit = static_cast<std::uint64_t>(limit),
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
                javelin::jmap::api::getRequestFrom(accountId, queryHandle, "/ids"));
            if (!representativeRequest.has_value())
            {
                co_return OperationError{
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
                co_return OperationError{
                    .message = QStringLiteral("Failed to encode the Thread/get request."),
                };
            }
            const auto threadHandle = builder.call(*threadRequest, "page-threads-get");

            const auto emailRequest = javelin::jmap::api::emailGet(
                javelin::jmap::api::getRequestFrom(accountId, threadHandle, "/list/*/emailIds"));
            if (!emailRequest.has_value())
            {
                co_return OperationError{
                    .message = QStringLiteral("Failed to encode the page Email/get request."),
                };
            }
            const auto emailHandle = builder.call(*emailRequest, "page-emails-get");

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
            if (reportProgress)
            {
                reportProgress(QStringLiteral("Fetched %1 matching conversations.")
                                   .arg(static_cast<qulonglong>(parsedQuery.ids.size())));
            }

            if (parsedQuery.ids.empty())
            {
                co_return CollapsedQueryPage{
                    .representativeCount = 0,
                    .position = static_cast<std::size_t>(parsedQuery.position),
                    .returnedLimit = static_cast<std::size_t>(parsedQuery.limit.value_or(limit)),
                    .total = parsedQuery.total.has_value()
                                 ? std::optional<std::size_t>{static_cast<std::size_t>(
                                       *parsedQuery.total)}
                                 : std::nullopt,
                    .queryState = parsedQuery.queryState,
                    .results = {},
                };
            }

            const auto representativeResult = reader.require(representativeHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&representativeResult))
            {
                co_return operationError(*error);
            }
            const auto& parsedRepresentatives =
                std::get<javelin::jmap::api::EmailGetResponse>(representativeResult);

            const auto threadResult = reader.require(threadHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&threadResult))
            {
                co_return operationError(*error);
            }
            const auto& parsedThreads =
                std::get<javelin::jmap::api::ThreadGetResponse>(threadResult);

            const auto emailResult = reader.require(emailHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&emailResult))
            {
                co_return operationError(*error);
            }
            const auto& parsedEmails = std::get<javelin::jmap::api::EmailGetResponse>(emailResult);

            javelin::jmap::cache::EmailRepository emailRepository{databaseConnection};
            if (const auto error = emailRepository.upsertMany(accountId, parsedEmails.list))
            {
                co_return javelin::jmap::operationError(*error);
            }

            std::vector<std::string> materializedEmailIds;
            materializedEmailIds.reserve(parsedEmails.list.size());
            for (const auto& email : parsedEmails.list)
                materializedEmailIds.push_back(email.id);
            if (const auto error = javelin::jmap::sync::rebaseActiveEmailProjections(
                    databaseConnection, accountId, std::move(materializedEmailIds),
                    parsedEmails.state))
            {
                co_return *error;
            }

            javelin::jmap::cache::ThreadRepository threadRepository{databaseConnection};
            if (const auto error = threadRepository.upsertMany(accountId, parsedThreads.list))
            {
                co_return javelin::jmap::operationError(*error);
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
            if (results.size() != parsedQuery.ids.size())
            {
                co_return OperationError{
                    .message =
                        QStringLiteral(
                            "Email/get omitted %1 representatives from the query window.")
                            .arg(static_cast<qulonglong>(parsedQuery.ids.size() - results.size())),
                };
            }

            co_return CollapsedQueryPage{
                .representativeCount = results.size(),
                .position = static_cast<std::size_t>(parsedQuery.position),
                .returnedLimit = static_cast<std::size_t>(parsedQuery.limit.value_or(limit)),
                .total =
                    parsedQuery.total.has_value()
                        ? std::optional<std::size_t>{static_cast<std::size_t>(*parsedQuery.total)}
                        : std::nullopt,
                .queryState = parsedQuery.queryState,
                .results = std::move(results),
            };
        }

    } // namespace

    JmapCore::JmapCore(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                       javelin::jmap::api::AbstractTransport& resourceTransport,
                       javelin::jmap::api::JmapMethodTransport& methodTransport)
        : m_impl(std::make_unique<Impl>())
    {
        m_impl->databaseConnection = &databaseConnection;
        m_impl->resourceTransport = &resourceTransport;
        m_impl->methodTransport = &methodTransport;
    }

    JmapCore::~JmapCore() = default;

    QCoro::Task<SessionRefreshResult> JmapCore::refreshSession(LiveConnectionSettings settings,
                                                               std::string ownerAccountId)
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
        if (ownerAccountId.empty())
        {
            co_return OperationError{
                .message = QStringLiteral("An owner account is required for session discovery."),
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
        if (const auto error = repository.replace(ownerAccountId, session))
        {
            co_return javelin::jmap::operationError(*error);
        }

        const bool websocketAdvertised = session.capabilities.websocket.has_value();
        co_return SessionRefreshSummary{
            .ownerAccountId = std::move(ownerAccountId),
            .resolvedSessionUrl = sessionClient.resolvedSessionUrl(),
            .websocketAdvertised = websocketAdvertised,
            .websocketPushSupported =
                websocketAdvertised && session.capabilities.websocket->supportsPush,
        };
    }

    QCoro::Task<LiveRefreshResult>
    JmapCore::refreshFromServer(LiveConnectionSettings settings,
                                std::function<void(const QString&)> progressCallback,
                                std::optional<std::vector<std::string>> configuredMailboxIds)
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

        javelin::jmap::api::SessionClient sessionClient{*m_impl->resourceTransport};

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

        const auto& accountId = *session.primaryAccounts.mailAccountId;
        javelin::jmap::cache::SessionRepository sessionRepository{*m_impl->databaseConnection};
        qInfo().noquote() << "JMAP core saving session and accounts"
                          << QString::fromStdString(accountId)
                          << static_cast<qulonglong>(session.accounts.size());
        if (const auto error = sessionRepository.replace(accountId, session))
        {
            co_return javelin::jmap::operationError(*error);
        }
        reportProgress(QStringLiteral("Cached session. Fetching mailboxes..."));
        const auto apiRequestContext = buildApiRequestContext(settings, accountId, session);
        qInfo().noquote() << "JMAP core mailbox request context ready"
                          << QString::fromStdString(apiRequestContext.apiUrl);

        javelin::jmap::api::MethodCaller methodCaller{*m_impl->methodTransport};
        const auto mailboxRequest = javelin::jmap::api::mailboxGet({.accountId = accountId,
                                                                    .ids = std::nullopt,
                                                                    .idsReference = std::nullopt,
                                                                    .properties = std::nullopt});
        if (!mailboxRequest.has_value())
        {
            co_return OperationError{
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
        reportProgress(QStringLiteral("Fetched %1 mailboxes. Updating cache...")
                           .arg(parsedMailboxes.list.size()));

        javelin::jmap::cache::MailboxRepository mailboxRepository{*m_impl->databaseConnection};
        if (const auto error = mailboxRepository.replaceAll(accountId, parsedMailboxes.list))
        {
            co_return javelin::jmap::operationError(*error);
        }

        javelin::jmap::cache::SyncStateRepository syncStateRepository{*m_impl->databaseConnection};
        if (const auto error = syncStateRepository.upsert(
                {.accountId = accountId, .objectType = "Mailbox", .queryKey = {}},
                parsedMailboxes.state))
        {
            co_return javelin::jmap::operationError(*error);
        }

        const auto selectedMailboxId = selectMailboxForInitialSync(parsedMailboxes.list);
        auto mailboxIds = configuredMailboxIds.value_or(std::vector<std::string>{});
        std::erase_if(mailboxIds,
                      [&parsedMailboxes](const auto& mailboxId)
                      {
                          return std::ranges::none_of(parsedMailboxes.list,
                                                      [&mailboxId](const auto& mailbox)
                                                      { return mailbox.id == mailboxId; });
                      });
        if (!configuredMailboxIds.has_value() && mailboxIds.empty() &&
            selectedMailboxId.has_value())
        {
            mailboxIds.push_back(*selectedMailboxId);
        }
        std::size_t emailCount = 0;

        if (!mailboxIds.empty())
        {
            javelin::jmap::sync::MailboxRefreshExecutor mailboxRefreshExecutor{
                *m_impl->databaseConnection, methodCaller, apiRequestContext};
            for (const auto& mailboxId : mailboxIds)
            {
                reportProgress(QStringLiteral("Refreshing selected mailbox..."));
                const auto refreshResult = co_await mailboxRefreshExecutor.refreshCollapsedMailbox(
                    accountId, mailboxId, reportProgress, true);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&refreshResult))
                {
                    co_return *error;
                }

                emailCount += std::get<javelin::jmap::sync::MailboxRefreshSummary>(refreshResult)
                                  .representativeCount;
            }
            reportProgress(QStringLiteral("Cached %1 threaded conversations.").arg(emailCount));
        }

        const auto refreshSummary = QStringLiteral("Loaded %1 mailboxes and %2 messages from %3.")
                                        .arg(parsedMailboxes.list.size())
                                        .arg(emailCount)
                                        .arg(QString::fromStdString(settings.loginEmail));
        qInfo().noquote() << "JMAP core refresh success" << refreshSummary;

        const auto pendingSubmit = co_await submitPendingEmailMutations(settings, accountId);
        if (const auto* summary = std::get_if<SubmittedEmailMutations>(&pendingSubmit);
            summary != nullptr && summary->updatedEmailCount > 0)
        {
            reportProgress(QStringLiteral("Submitted %1 queued mailbox updates.")
                               .arg(summary->updatedEmailCount));
        }

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

        const auto cachedSource = sourceRepository.find(accountId, emailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&cachedSource))
        {
            co_return operationError(*error);
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
        const auto downloadResult = co_await downloadBlob(
            *m_impl->resourceTransport, context.session.downloadUrl, accountId, sourcePart,
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

        auto payload = std::get<QByteArray>(std::move(downloadResult));
        const auto payloadSize = payload.size();
        auto storeFuture = QtConcurrent::run(storeRawMessageSource,
                                             m_impl->databaseConnection->database().databaseName(),
                                             accountId, email.id, email.blobId, std::move(payload));
        const QString storeError = co_await qCoro(storeFuture).takeResult();
        if (!storeError.isEmpty())
            co_return OperationError{.message = storeError};

        qInfo().noquote() << "JMAP core message source refresh success"
                          << QString::fromStdString(emailId)
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

    QCoro::Task<FullMailboxPageResult>
    JmapCore::materializeFullMailboxPage(LiveConnectionSettings settings, std::string accountId,
                                         std::string mailboxId, const std::size_t position,
                                         const std::size_t limit, std::optional<std::string> anchor)
    {
        if (m_impl->databaseConnection == nullptr || m_impl->methodTransport == nullptr)
        {
            co_return OperationError{
                .message = QStringLiteral("Full mailbox synchronization is unavailable."),
            };
        }
        if (const auto validationError = validateLoginSettings(settings, true))
            co_return *validationError;
        const auto sessionResult = loadCachedSession(*m_impl->databaseConnection, accountId);
        if (const auto* error = std::get_if<OperationError>(&sessionResult))
            co_return *error;

        javelin::jmap::api::MethodCaller caller{*m_impl->methodTransport};
        javelin::jmap::api::RequestBuilder builder;
        builder.useCore().useMail();
        const auto queryRequest = javelin::jmap::api::emailQuery({
            .accountId = accountId,
            .filter = javelin::jmap::api::EmailQueryFilter{.inMailbox = mailboxId},
            .sort = {javelin::jmap::api::EmailQuerySort{.property = "receivedAt",
                                                        .isAscending = false}},
            .position = anchor.has_value()
                            ? std::nullopt
                            : std::optional<std::uint64_t>{static_cast<std::uint64_t>(position)},
            .anchor = std::move(anchor),
            .anchorOffset = static_cast<std::int64_t>(position),
            .limit = static_cast<std::uint64_t>(limit),
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
            accountId, queryHandle, "/ids",
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
            buildApiRequestContext(settings, accountId,
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
        const auto getResult = reader.require(getHandle);
        if (const auto* error = std::get_if<javelin::jmap::api::ResponseReaderError>(&getResult))
            co_return operationError(*error);

        const auto& page = std::get<javelin::jmap::api::EmailQueryResponse>(queryResult);
        const auto& emails = std::get<javelin::jmap::api::EmailGetResponse>(getResult);
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
    JmapCore::downloadAttachment(LiveConnectionSettings settings, std::string accountId,
                                 std::string emailId, std::string partId)
    {
        Q_UNUSED(settings);

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
    JmapCore::loadCachedMessageSource(std::string accountId, std::string emailId)
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

    QueuedEmailMutationResult JmapCore::queueMoveEmail(std::string accountId, std::string emailId,
                                                       std::string sourceMailboxId,
                                                       std::string destinationMailboxId)
    {
        if (m_impl->databaseConnection == nullptr)
        {
            return OperationError{
                .message = QStringLiteral("Queued mutations are unavailable in this process."),
            };
        }

        return queueMailboxPatch(*m_impl->databaseConnection, std::move(accountId),
                                 std::move(emailId), std::move(sourceMailboxId),
                                 std::move(destinationMailboxId), true);
    }

    QueuedEmailMutationResult JmapCore::queueEmailMailboxMutation(std::string accountId,
                                                                  EmailMailboxMutation mutation)
    {
        if (m_impl->databaseConnection == nullptr)
        {
            return OperationError{
                .message = QStringLiteral("Queued mutations are unavailable in this process."),
            };
        }

        return ::javelin::jmap::queueEmailPatch(*m_impl->databaseConnection, std::move(accountId),
                                                std::move(mutation));
    }

    QueuedEmailMutationResult JmapCore::queueCopyEmail(std::string accountId, std::string emailId,
                                                       std::string sourceMailboxId,
                                                       std::string destinationMailboxId)
    {
        if (m_impl->databaseConnection == nullptr)
        {
            return OperationError{
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

    QueuedEmailMutationResult
    JmapCore::queueDestroyEmail(std::string accountId, std::string emailId,
                                std::optional<std::string> operationGroupId)
    {
        if (m_impl->databaseConnection == nullptr)
        {
            return OperationError{
                .message = QStringLiteral("Queued mutations are unavailable in this process."),
            };
        }

        return queueDestroyEmailMutation(*m_impl->databaseConnection, std::move(accountId),
                                         std::move(emailId), std::move(operationGroupId));
    }

    QueuedEmailMutationResult JmapCore::queueMarkEmailRead(std::string accountId,
                                                           std::string emailId)
    {
        if (m_impl->databaseConnection == nullptr)
        {
            return OperationError{
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
            return OperationError{
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
            return OperationError{
                .message = QStringLiteral("Queued mutations are unavailable in this process."),
            };
        }

        return queueKeywordPatch(*m_impl->databaseConnection, std::move(accountId),
                                 std::move(emailId), "$flagged", flagged);
    }

    QCoro::Task<SubmittedEmailMutationsResult>
    JmapCore::submitPendingEmailMutations(LiveConnectionSettings settings, std::string accountId,
                                          std::optional<std::string> operationGroupId,
                                          const std::size_t limit)
    {
        if (m_impl->databaseConnection == nullptr || m_impl->methodTransport == nullptr)
        {
            co_return OperationError{
                .message = QStringLiteral("JMAP core is not wired to the cache and transport yet."),
            };
        }

        if (const auto validationError = validateLoginSettings(settings, true))
        {
            co_return *validationError;
        }

        const auto sessionResult = loadCachedSession(*m_impl->databaseConnection, accountId);
        if (const auto* error = std::get_if<OperationError>(&sessionResult))
        {
            co_return *error;
        }
        const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);

        javelin::jmap::sync::EmailMutationJournal emailMutationJournal{*m_impl->databaseConnection};
        auto pendingResult =
            operationGroupId.has_value()
                ? emailMutationJournal.listForOperationGroup(accountId, *operationGroupId)
                : emailMutationJournal.listByStatus(
                      accountId, javelin::jmap::sync::MutationStatus::Pending, limit);
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
                .items = {},
            };
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
        for (const auto& emailId : emailIds)
        {
            const auto emailResult = emailRepository.find(accountId, emailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&emailResult))
            {
                co_return javelin::jmap::operationError(*error);
            }

            const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(emailResult);
            if (!email.has_value())
            {
                co_return OperationError{
                    .message = QStringLiteral("A queued email mutation targets a missing email."),
                };
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
            {
                continue;
            }
            std::vector<javelin::jmap::sync::EmailMutationRecord> selectedActions;
            for (const auto& action : pendingActions)
            {
                if (action.patch.emailId == emailId)
                    selectedActions.push_back(action);
            }
            mutationsByEmailId.emplace(emailId, selectedActions);

            mergedEmails.emplace(
                emailId, javelin::jmap::sync::projectEmailMutations(*email, allEmailActions));

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

        std::optional<std::string> ifInState;
        for (const auto& action : pendingActions)
        {
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

        const auto requestMethod = javelin::jmap::api::emailSet({
            .accountId = accountId,
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
        const auto apiRequestContext = buildApiRequestContext(settings, accountId, session);
        javelin::jmap::api::RequestBuilder requestBuilder;
        requestBuilder.useCore().useMail();
        const auto setHandle = requestBuilder.call(*requestMethod, "queued-email-set");

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
            [this, &accountId, &mergedEmails, &mutationsByEmailId,
             &mutationIdsByEmailId]() -> std::optional<OperationError>
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
                if (const auto error =
                        emails.upsertMany(transaction.cacheTransaction(), accountId, {restored}))
                {
                    return javelin::jmap::operationError(*error);
                }
                for (const auto& mutationId : mutationIdsByEmailId.at(emailId))
                {
                    if (const auto error = transaction.transition(
                            mutationId, javelin::jmap::sync::MutationStatus::Rejected))
                    {
                        return javelin::jmap::operationError(*error);
                    }
                }
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

                if (destroyedEmailIds.contains(emailId))
                {
                    const std::array destroyed{emailId};
                    if (const auto error = emailRepository.removeMany(
                            transaction.cacheTransaction(), accountId, destroyed))
                    {
                        co_return javelin::jmap::operationError(*error);
                    }
                }
                else if (const auto error = emailRepository.upsertMany(
                             transaction.cacheTransaction(), accountId, {email}))
                {
                    co_return javelin::jmap::operationError(*error);
                }

                for (const auto& mutationId : idsIt->second)
                {
                    if (const auto error = transaction.remove(mutationId))
                    {
                        co_return javelin::jmap::operationError(*error);
                    }
                }
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
                if (const auto error = emailRepository.upsertMany(transaction.cacheTransaction(),
                                                                  accountId, {restored}))
                {
                    co_return javelin::jmap::operationError(*error);
                }
            }
            for (const auto& mutationId : idsIt->second)
            {
                const auto status = failedEmailIds.contains(emailId)
                                        ? javelin::jmap::sync::MutationStatus::Rejected
                                        : javelin::jmap::sync::MutationStatus::Pending;
                if (const auto error = transaction.transition(mutationId, status))
                {
                    co_return javelin::jmap::operationError(*error);
                }
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
            .accountId = std::move(accountId),
            .attemptedEmailCount = mergedEmails.size(),
            .updatedEmailCount = updatedEmailIds.size() + destroyedEmailIds.size(),
            .failedEmailCount = failedEmailIds.size(),
            .items = std::move(items),
        };
    }

    QCoro::Task<AuthoritativeEmailsResult>
    JmapCore::getAuthoritativeEmails(LiveConnectionSettings settings, std::string accountId,
                                     std::vector<std::string> emailIds)
    {
        if (m_impl->databaseConnection == nullptr || m_impl->methodTransport == nullptr)
        {
            co_return OperationError{
                .message = QStringLiteral("JMAP core is not wired to the cache and transport yet."),
            };
        }
        if (const auto validationError = validateLoginSettings(settings, true))
            co_return *validationError;

        const auto sessionResult = loadCachedSession(*m_impl->databaseConnection, accountId);
        if (const auto* error = std::get_if<OperationError>(&sessionResult))
            co_return *error;
        const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);

        const auto getMethod = javelin::jmap::api::emailGet({
            .accountId = accountId,
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
        const auto result =
            co_await caller.call(buildApiRequestContext(settings, accountId, session), builder);
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
        co_return AuthoritativeEmails{
            .accountId = response.accountId,
            .state = response.state,
            .emails = response.list,
            .notFound = response.notFound,
        };
    }

    QCoro::Task<MessageSearchResult> JmapCore::searchMessages(
        LiveConnectionSettings settings, std::string accountId, std::string query,
        const std::size_t offset, const std::size_t limit, javelin::jmap::query::EmailListSort sort,
        std::optional<std::string> anchor, std::optional<std::string> windowKey,
        std::function<void(const QString&)> progressCallback)
    {
        co_return co_await searchMessages(
            std::move(settings), std::move(accountId),
            javelin::jmap::search::EmailSearchCriteria{.text = std::move(query)}, offset, limit,
            std::move(sort), std::move(anchor), std::move(windowKey), std::move(progressCallback));
    }

    QCoro::Task<MessageSearchResult> JmapCore::searchMessages(
        LiveConnectionSettings settings, std::string accountId,
        javelin::jmap::search::EmailSearchCriteria criteria, const std::size_t offset,
        const std::size_t limit, javelin::jmap::query::EmailListSort sort,
        std::optional<std::string> anchor, std::optional<std::string> windowKey,
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
        const auto queryKey = windowKey.value_or(javelin::jmap::search::cacheKey(criteria, sort));
        qInfo().noquote() << "JMAP core search start" << QString::fromStdString(accountId)
                          << QString::fromStdString(query);
        reportProgress(QStringLiteral("Searching the server..."));
        if (m_impl->databaseConnection == nullptr || m_impl->methodTransport == nullptr)
        {
            co_return OperationError{
                .message = QStringLiteral("JMAP core is not wired to the cache and transport yet."),
            };
        }

        if (javelin::jmap::search::isEmpty(criteria))
        {
            co_return OperationError{
                .message = QStringLiteral("Enter a search term before searching."),
            };
        }

        const auto pageResult = co_await performCollapsedQueryPage(
            *m_impl->databaseConnection, *m_impl->methodTransport, settings, accountId,
            javelin::jmap::search::toEmailQueryFilter(criteria), offset, limit, std::move(sort),
            std::move(anchor), 1, reportProgress);
        if (const auto* error = std::get_if<OperationError>(&pageResult))
        {
            co_return *error;
        }

        auto page = std::get<CollapsedQueryPage>(std::move(pageResult));
        std::vector<std::string> emailIds;
        emailIds.reserve(page.results.size());
        for (const auto& item : page.results)
        {
            emailIds.push_back(item.emailId);
        }
        javelin::jmap::cache::SearchWindowRepository searchWindowRepository{
            *m_impl->databaseConnection};
        if (const auto error = searchWindowRepository.replace({
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
        {
            co_return javelin::jmap::operationError(*error);
        }

        javelin::jmap::cache::QueryService queryService{*m_impl->databaseConnection};
        const auto cachedResults = queryService.listMessagesByEmailIds(accountId, emailIds);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&cachedResults))
        {
            co_return javelin::jmap::operationError(*error);
        }
        page.results = std::get<std::vector<javelin::jmap::cache::MessageListItem>>(cachedResults);

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
            .results = std::move(page.results),
        };
    }

    QCoro::Task<MailboxPageResult>
    JmapCore::queryMailboxPage(LiveConnectionSettings settings, std::string accountId,
                               std::string mailboxId, const std::size_t offset,
                               const std::size_t limit, javelin::jmap::query::EmailListSort sort,
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

        qInfo().noquote() << "JMAP core mailbox page query" << QString::fromStdString(accountId)
                          << QString::fromStdString(mailboxId) << static_cast<qulonglong>(offset)
                          << static_cast<qulonglong>(limit);
        reportProgress(QStringLiteral("Fetching mailbox page from the server..."));
        if (m_impl->databaseConnection == nullptr || m_impl->methodTransport == nullptr)
        {
            co_return OperationError{
                .message = QStringLiteral("JMAP core is not wired to the cache and transport yet."),
            };
        }

        const bool anchoredRequest = anchor.has_value();
        const auto pageResult = co_await performCollapsedQueryPage(
            *m_impl->databaseConnection, *m_impl->methodTransport, settings, accountId,
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
        std::vector<std::string> representativeIds;
        representativeIds.reserve(page.results.size());
        for (const auto& item : page.results)
            representativeIds.push_back(item.emailId);
        const auto queryKey = javelin::jmap::sync::mailboxQueryKey({
            .mailboxId = mailboxId,
            .sortProperty = javelin::jmap::query::propertyName(sort.property),
            .isAscending = javelin::jmap::query::isAscending(sort),
            .collapseThreads = true,
        });
        javelin::jmap::cache::MailboxWindowRepository windowRepository{*m_impl->databaseConnection};
        if (const auto error = windowRepository.replace({
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
        {
            co_return javelin::jmap::operationError(*error);
        }
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
            .results = std::move(page.results),
        };
    }

} // namespace javelin::jmap
