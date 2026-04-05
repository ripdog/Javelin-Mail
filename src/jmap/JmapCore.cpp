#include "jmap/JmapCore.h"

#include "jmap/api/Error.h"
#include "jmap/api/MailMethods.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/MethodEnvelope.h"
#include "jmap/api/SessionClient.h"
#include "jmap/api/Transport.h"
#include "jmap/auth/AccessTokenResolver.h"
#include "jmap/auth/Auth.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/InlinePartPayloadRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/MessageContentRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/SyncStateRepository.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>
#include <algorithm>
#include <unordered_map>
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
            return QStringLiteral("Transport error (%1): %2")
                .arg(QString::fromUtf8(javelin::jmap::api::toString(error.code).data()),
                     QString::fromStdString(error.message));
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

        [[nodiscard]] std::optional<javelin::jmap::api::MethodInvocation>
        findMethodResponse(const javelin::jmap::api::ResponseEnvelope& envelope,
                           const std::string_view expectedName, const std::string_view callId)
        {
            for (const auto& methodResponse : envelope.methodResponses)
            {
                if (methodResponse.callId == callId && methodResponse.name == expectedName)
                {
                    return methodResponse;
                }
            }

            return std::nullopt;
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

        [[nodiscard]] std::optional<QString> dumpRawJsonToTempFile(const QString& stem,
                                                                   std::string_view json)
        {
            const QString tempPath = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
            if (tempPath.isEmpty())
            {
                return std::nullopt;
            }

            QDir directory{tempPath};
            const QString fileName = QStringLiteral("javelin-mail-%1-%2.json")
                                         .arg(stem, QDateTime::currentDateTimeUtc().toString(
                                                        QStringLiteral("yyyyMMddHHmmsszzz")));
            const QString filePath = directory.filePath(fileName);

            QFile file{filePath};
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            {
                qWarning().noquote()
                    << "JMAP raw JSON dump failed" << filePath << file.errorString();
                return std::nullopt;
            }

            const auto written = file.write(json.data(), static_cast<qint64>(json.size()));
            if (written != static_cast<qint64>(json.size()) || !file.flush())
            {
                qWarning().noquote()
                    << "JMAP raw JSON dump incomplete" << filePath << file.errorString();
                return std::nullopt;
            }

            qWarning().noquote() << "JMAP raw JSON dumped" << filePath;
            return filePath;
        }

        [[nodiscard]] std::optional<std::string>
        normalizedPartId(const javelin::jmap::api::EmailContentBodyPart& part)
        {
            if (!part.partId.has_value() || part.partId->empty())
            {
                return std::nullopt;
            }

            return part.partId;
        }

        void upsertContentPart(
            std::unordered_map<std::string, javelin::jmap::cache::EmailPart>& partsById,
            const std::string& emailId, const javelin::jmap::api::EmailContentBodyPart& part,
            const bool isBodySection)
        {
            const auto partId = normalizedPartId(part);
            if (!partId.has_value())
            {
                return;
            }

            const bool isInlineRenderable = part.type.rfind("image/", 0) == 0 ||
                                            part.type.rfind("audio/", 0) == 0 ||
                                            part.type.rfind("video/", 0) == 0;

            auto [iterator, inserted] =
                partsById.try_emplace(*partId, javelin::jmap::cache::EmailPart{
                                                   .emailId = emailId,
                                                   .partId = *partId,
                                                   .parentPartId = std::nullopt,
                                                   .blobId = part.blobId,
                                                   .kind = isBodySection ? "body" : "attachment",
                                                   .mediaType = part.type,
                                                   .name = part.name,
                                                   .charset = part.charset,
                                                   .disposition = part.disposition,
                                                   .cid = part.cid,
                                                   .size = part.size,
                                                   .isInlineRenderable = isInlineRenderable,
                                                   .isBodySection = isBodySection,
                                               });
            if (!inserted)
            {
                iterator->second.blobId = part.blobId;
                iterator->second.mediaType = part.type;
                iterator->second.name = part.name;
                iterator->second.charset = part.charset;
                iterator->second.disposition = part.disposition;
                iterator->second.cid = part.cid;
                iterator->second.size = part.size;
                iterator->second.isInlineRenderable =
                    iterator->second.isInlineRenderable || isInlineRenderable;
                iterator->second.isBodySection = iterator->second.isBodySection || isBodySection;
            }
        }

        [[nodiscard]] std::vector<javelin::jmap::cache::EmailPart>
        buildContentParts(const javelin::jmap::api::EmailContent& content)
        {
            std::unordered_map<std::string, javelin::jmap::cache::EmailPart> partsById;
            for (const auto& part : content.textBody)
            {
                upsertContentPart(partsById, content.id, part, true);
            }
            for (const auto& part : content.htmlBody)
            {
                upsertContentPart(partsById, content.id, part, true);
            }
            for (const auto& part : content.attachments)
            {
                upsertContentPart(partsById, content.id, part, false);
            }

            std::vector<javelin::jmap::cache::EmailPart> parts;
            parts.reserve(partsById.size());
            for (auto& [ignoredPartId, part] : partsById)
            {
                Q_UNUSED(ignoredPartId);
                parts.push_back(std::move(part));
            }

            std::ranges::sort(parts, [](const auto& left, const auto& right)
                              { return left.partId < right.partId; });
            return parts;
        }

        [[nodiscard]] std::vector<javelin::jmap::cache::EmailBodyValue>
        buildBodyValues(const javelin::jmap::api::EmailContent& content)
        {
            std::vector<javelin::jmap::cache::EmailBodyValue> values;
            values.reserve(content.bodyValues.size());

            for (const auto& [partId, bodyValue] : content.bodyValues)
            {
                values.push_back(javelin::jmap::cache::EmailBodyValue{
                    .emailId = content.id,
                    .partId = partId,
                    .blobId = std::nullopt,
                    .isTruncated = bodyValue.isTruncated,
                    .value = bodyValue.value,
                });
            }

            std::ranges::sort(values, [](const auto& left, const auto& right)
                              { return left.partId < right.partId; });
            return values;
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

        [[nodiscard]] std::vector<javelin::jmap::cache::EmailPart> missingInlineImageParts(
            const std::vector<javelin::jmap::cache::EmailPart>& parts,
            javelin::jmap::cache::InlinePartPayloadRepository& payloadRepository,
            const std::string_view accountId)
        {
            std::vector<javelin::jmap::cache::EmailPart> missingParts;
            for (const auto& part : parts)
            {
                if (!part.isInlineRenderable || !part.blobId.has_value() || !part.cid.has_value() ||
                    part.mediaType.rfind("image/", 0) != 0)
                {
                    continue;
                }

                const auto payloadResult =
                    payloadRepository.find(accountId, part.emailId, part.partId);
                if (std::holds_alternative<javelin::jmap::cache::DatabaseError>(payloadResult))
                {
                    continue;
                }

                const auto& payload =
                    std::get<std::optional<javelin::jmap::cache::InlinePartPayload>>(payloadResult);
                if (payload.has_value() && payload->blobId == *part.blobId)
                {
                    continue;
                }

                missingParts.push_back(part);
            }

            return missingParts;
        }

        [[nodiscard]] QCoro::Task<std::optional<LiveRefreshError>>
        cacheInlineImageParts(javelin::jmap::api::AbstractTransport& transport,
                              const javelin::jmap::auth::AccountCredentials& credentials,
                              const std::string_view downloadUrlTemplate,
                              const std::string_view accountId,
                              const std::vector<javelin::jmap::cache::EmailPart>& parts,
                              javelin::jmap::cache::InlinePartPayloadRepository& payloadRepository)
        {
            const javelin::jmap::auth::AccessTokenResolver accessTokenResolver;
            const auto tokenResult = accessTokenResolver.resolve(credentials);
            if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&tokenResult))
            {
                co_return LiveRefreshError{.message = authMessage(*error)};
            }

            const auto accessToken =
                std::get<javelin::jmap::auth::OAuthToken>(tokenResult).accessToken;
            for (const auto& part : parts)
            {
                const auto transportResult = co_await transport.send(buildDownloadRequest(
                    buildDownloadUrl(downloadUrlTemplate, accountId, part), accessToken));
                if (const auto* error =
                        std::get_if<javelin::jmap::api::TransportError>(&transportResult))
                {
                    co_return LiveRefreshError{.message = transportMessage(*error)};
                }

                const auto& response = std::get<javelin::jmap::api::HttpResponse>(transportResult);
                if (const auto error =
                        payloadRepository.upsert(accountId, {
                                                                .emailId = part.emailId,
                                                                .partId = part.partId,
                                                                .blobId = *part.blobId,
                                                                .mediaType = part.mediaType,
                                                                .payload = response.body,
                                                            }))
                {
                    co_return LiveRefreshError{.message = error->message};
                }
            }

            co_return std::nullopt;
        }

        [[nodiscard]] std::optional<javelin::jmap::cache::EmailPart>
        findAttachmentPart(javelin::jmap::cache::MessageContentRepository& contentRepository,
                           const std::string_view accountId, const std::string_view emailId,
                           const std::string_view partId, QString& errorMessage)
        {
            const auto partsResult = contentRepository.loadParts(accountId, emailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&partsResult))
            {
                errorMessage = error->message;
                return std::nullopt;
            }

            const auto& parts = std::get<std::vector<javelin::jmap::cache::EmailPart>>(partsResult);
            const auto partIt = std::ranges::find(parts, std::string{partId},
                                                  &javelin::jmap::cache::EmailPart::partId);
            if (partIt == parts.end())
            {
                errorMessage = QStringLiteral("The selected attachment is not cached locally.");
                return std::nullopt;
            }

            if (!partIt->blobId.has_value() || partIt->blobId->empty())
            {
                errorMessage = QStringLiteral("The selected attachment does not expose a blob id.");
                return std::nullopt;
            }

            return *partIt;
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

        if (settings.sessionUrl.empty() || settings.loginEmail.empty() || settings.apiKey.empty())
        {
            co_return LiveRefreshError{
                .message = QStringLiteral("Session URL, login email, and API key are required."),
            };
        }

        javelin::jmap::api::SessionClient sessionClient{*m_impl->transport};

        const javelin::jmap::auth::SessionRequestContext sessionRequestContext{
            .credentials =
                {
                    .accountId = settings.loginEmail,
                    .emailAddress = settings.loginEmail,
                    .sessionUrl = settings.sessionUrl,
                    .token =
                        {
                            .accessToken = settings.apiKey,
                            .refreshToken = std::nullopt,
                            .expiry = std::nullopt,
                        },
                },
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
            co_return LiveRefreshError{.message = authMessage(*error)};
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
        qInfo() << "JMAP core after cached-session progress";
        qInfo() << "JMAP core before api request context";

        qInfo() << "JMAP core building api request context";
        javelin::jmap::api::ApiRequestContext apiRequestContext{
            .credentials =
                {
                    .accountId = {},
                    .emailAddress = {},
                    .sessionUrl = {},
                    .token =
                        {
                            .accessToken = {},
                            .refreshToken = std::nullopt,
                            .expiry = std::nullopt,
                        },
                },
            .apiUrl = {},
        };
        qInfo() << "JMAP core request context default constructed";
        apiRequestContext.credentials.accountId = accountId;
        qInfo() << "JMAP core request context account set";
        apiRequestContext.credentials.emailAddress = settings.loginEmail;
        qInfo() << "JMAP core request context email set";
        apiRequestContext.credentials.sessionUrl = settings.sessionUrl;
        qInfo() << "JMAP core request context session url set";
        apiRequestContext.credentials.token.accessToken = settings.apiKey;
        qInfo() << "JMAP core request context token set";
        apiRequestContext.apiUrl = session.apiUrl;
        qInfo().noquote() << "JMAP core mailbox request context ready"
                          << QString::fromStdString(apiRequestContext.apiUrl);

        qInfo() << "JMAP core before method caller construction";
        javelin::jmap::api::MethodCaller methodCaller{*m_impl->transport};
        qInfo() << "JMAP core method caller ready";

        qInfo() << "JMAP core serializing Mailbox/get request";
        const auto mailboxArguments = javelin::jmap::api::serializeGetRequest(
            {.accountId = accountId, .ids = std::nullopt, .properties = std::nullopt});
        if (!mailboxArguments.has_value())
        {
            co_return LiveRefreshError{
                .message = QStringLiteral("Failed to encode the Mailbox/get request."),
            };
        }
        qInfo().noquote() << "JMAP core Mailbox/get request encoded"
                          << QString::fromStdString(*mailboxArguments);

        const javelin::jmap::api::RequestEnvelope mailboxRequest{
            .usingCapabilities =
                {
                    std::string{javelin::jmap::api::coreCapabilityUri},
                    std::string{javelin::jmap::api::mailCapabilityUri},
                },
            .methodCalls =
                {
                    javelin::jmap::api::MethodInvocation{
                        .name = "Mailbox/get",
                        .arguments = *mailboxArguments,
                        .callId = "mailboxes",
                    },
                },
            .createdIds = std::nullopt,
        };
        qInfo() << "JMAP core Mailbox/get envelope ready";
        qInfo() << "JMAP core before Mailbox/get await";

        const auto mailboxEnvelopeResult =
            co_await methodCaller.call(apiRequestContext, mailboxRequest);
        if (const auto* error =
                std::get_if<javelin::jmap::api::TransportError>(&mailboxEnvelopeResult))
        {
            co_return LiveRefreshError{.message = transportMessage(*error)};
        }
        if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&mailboxEnvelopeResult))
        {
            co_return LiveRefreshError{.message = authMessage(*error)};
        }
        if (const auto* error =
                std::get_if<javelin::jmap::api::ProtocolError>(&mailboxEnvelopeResult))
        {
            co_return LiveRefreshError{.message = protocolMessage(*error)};
        }

        const auto& mailboxEnvelope =
            std::get<javelin::jmap::api::ResponseEnvelope>(mailboxEnvelopeResult);
        const auto mailboxMethod = findMethodResponse(mailboxEnvelope, "Mailbox/get", "mailboxes");
        if (!mailboxMethod.has_value())
        {
            co_return LiveRefreshError{
                .message =
                    QStringLiteral("The server response did not contain Mailbox/get results."),
            };
        }

        const auto parsedMailboxes =
            javelin::jmap::api::parseMailboxGetResponse(mailboxMethod->arguments);
        if (!parsedMailboxes.ok() || !parsedMailboxes.value.has_value())
        {
            const auto dumped =
                dumpRawJsonToTempFile(QStringLiteral("mailbox-get"), mailboxMethod->arguments);
            co_return LiveRefreshError{
                .message = dumped.has_value()
                               ? QStringLiteral("Failed to parse Mailbox/get response. Raw JSON "
                                                "dumped to %1.")
                                     .arg(*dumped)
                               : QStringLiteral("Failed to parse Mailbox/get response."),
            };
        }
        reportProgress(QStringLiteral("Fetched %1 mailboxes. Updating cache...")
                           .arg(parsedMailboxes.value->list.size()));

        javelin::jmap::cache::MailboxRepository mailboxRepository{*m_impl->databaseConnection};
        if (const auto error = mailboxRepository.replaceAll(accountId, parsedMailboxes.value->list))
        {
            co_return LiveRefreshError{.message = error->message};
        }

        javelin::jmap::cache::SyncStateRepository syncStateRepository{*m_impl->databaseConnection};
        if (const auto error = syncStateRepository.upsert(
                {.accountId = accountId, .objectType = "Mailbox", .queryKey = {}},
                parsedMailboxes.value->state))
        {
            co_return LiveRefreshError{.message = error->message};
        }

        const auto selectedMailboxId = selectMailboxForInitialSync(parsedMailboxes.value->list);
        std::size_t emailCount = 0;

        if (selectedMailboxId.has_value())
        {
            reportProgress(QStringLiteral("Fetching message list..."));
            const auto queryArguments = javelin::jmap::api::serializeEmailQueryRequest({
                .accountId = accountId,
                .filter =
                    javelin::jmap::api::EmailQueryFilter{
                        .inMailbox = *selectedMailboxId,
                    },
                .sort =
                    {
                        javelin::jmap::api::EmailQuerySort{
                            .property = "receivedAt",
                            .isAscending = false,
                        },
                    },
                .position = 0,
                .limit = 100,
                .collapseThreads = false,
                .calculateTotal = false,
            });
            if (!queryArguments.has_value())
            {
                co_return LiveRefreshError{
                    .message = QStringLiteral("Failed to encode the Email/query request."),
                };
            }

            const javelin::jmap::api::RequestEnvelope queryRequest{
                .usingCapabilities =
                    {
                        std::string{javelin::jmap::api::coreCapabilityUri},
                        std::string{javelin::jmap::api::mailCapabilityUri},
                    },
                .methodCalls =
                    {
                        javelin::jmap::api::MethodInvocation{
                            .name = "Email/query",
                            .arguments = *queryArguments,
                            .callId = "emails-query",
                        },
                    },
                .createdIds = std::nullopt,
            };

            const auto queryEnvelopeResult =
                co_await methodCaller.call(apiRequestContext, queryRequest);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::TransportError>(&queryEnvelopeResult))
            {
                co_return LiveRefreshError{.message = transportMessage(*error)};
            }
            if (const auto* error =
                    std::get_if<javelin::jmap::api::AuthError>(&queryEnvelopeResult))
            {
                co_return LiveRefreshError{.message = authMessage(*error)};
            }
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ProtocolError>(&queryEnvelopeResult))
            {
                co_return LiveRefreshError{.message = protocolMessage(*error)};
            }

            const auto& queryEnvelope =
                std::get<javelin::jmap::api::ResponseEnvelope>(queryEnvelopeResult);
            const auto queryMethod =
                findMethodResponse(queryEnvelope, "Email/query", "emails-query");
            if (!queryMethod.has_value())
            {
                co_return LiveRefreshError{
                    .message =
                        QStringLiteral("The server response did not contain Email/query results."),
                };
            }

            const auto parsedQuery =
                javelin::jmap::api::parseEmailQueryResponse(queryMethod->arguments);
            if (!parsedQuery.ok() || !parsedQuery.value.has_value())
            {
                const auto dumped =
                    dumpRawJsonToTempFile(QStringLiteral("email-query"), queryMethod->arguments);
                co_return LiveRefreshError{
                    .message = dumped.has_value()
                                   ? QStringLiteral("Failed to parse Email/query response. Raw "
                                                    "JSON dumped to %1.")
                                         .arg(*dumped)
                                   : QStringLiteral("Failed to parse Email/query response."),
                };
            }
            reportProgress(
                QStringLiteral("Fetched %1 message ids.").arg(parsedQuery.value->ids.size()));

            if (const auto error = syncStateRepository.upsert({.accountId = accountId,
                                                               .objectType = "EmailQuery",
                                                               .queryKey = *selectedMailboxId},
                                                              parsedQuery.value->queryState))
            {
                co_return LiveRefreshError{.message = error->message};
            }

            if (!parsedQuery.value->ids.empty())
            {
                reportProgress(QStringLiteral("Fetching message summaries..."));
                const auto emailArguments = javelin::jmap::api::serializeGetRequest({
                    .accountId = accountId,
                    .ids = parsedQuery.value->ids,
                    .properties = std::nullopt,
                });
                if (!emailArguments.has_value())
                {
                    co_return LiveRefreshError{
                        .message = QStringLiteral("Failed to encode the Email/get request."),
                    };
                }

                const javelin::jmap::api::RequestEnvelope emailRequest{
                    .usingCapabilities =
                        {
                            std::string{javelin::jmap::api::coreCapabilityUri},
                            std::string{javelin::jmap::api::mailCapabilityUri},
                        },
                    .methodCalls =
                        {
                            javelin::jmap::api::MethodInvocation{
                                .name = "Email/get",
                                .arguments = *emailArguments,
                                .callId = "emails-get",
                            },
                        },
                    .createdIds = std::nullopt,
                };

                const auto emailEnvelopeResult =
                    co_await methodCaller.call(apiRequestContext, emailRequest);
                if (const auto* error =
                        std::get_if<javelin::jmap::api::TransportError>(&emailEnvelopeResult))
                {
                    co_return LiveRefreshError{.message = transportMessage(*error)};
                }
                if (const auto* error =
                        std::get_if<javelin::jmap::api::AuthError>(&emailEnvelopeResult))
                {
                    co_return LiveRefreshError{.message = authMessage(*error)};
                }
                if (const auto* error =
                        std::get_if<javelin::jmap::api::ProtocolError>(&emailEnvelopeResult))
                {
                    co_return LiveRefreshError{.message = protocolMessage(*error)};
                }

                const auto& emailEnvelope =
                    std::get<javelin::jmap::api::ResponseEnvelope>(emailEnvelopeResult);
                const auto emailMethod =
                    findMethodResponse(emailEnvelope, "Email/get", "emails-get");
                if (!emailMethod.has_value())
                {
                    co_return LiveRefreshError{
                        .message = QStringLiteral(
                            "The server response did not contain Email/get results."),
                    };
                }

                const auto parsedEmails =
                    javelin::jmap::api::parseEmailGetResponse(emailMethod->arguments);
                if (!parsedEmails.ok() || !parsedEmails.value.has_value())
                {
                    const auto dumped =
                        dumpRawJsonToTempFile(QStringLiteral("email-get"), emailMethod->arguments);
                    co_return LiveRefreshError{
                        .message = dumped.has_value()
                                       ? QStringLiteral("Failed to parse Email/get response. Raw "
                                                        "JSON dumped to %1.")
                                             .arg(*dumped)
                                       : QStringLiteral("Failed to parse Email/get response."),
                    };
                }

                javelin::jmap::cache::EmailRepository emailRepository{*m_impl->databaseConnection};
                if (const auto error =
                        emailRepository.replaceAll(accountId, parsedEmails.value->list))
                {
                    co_return LiveRefreshError{.message = error->message};
                }

                if (const auto error = syncStateRepository.upsert(
                        {.accountId = accountId, .objectType = "Email", .queryKey = {}},
                        parsedEmails.value->state))
                {
                    co_return LiveRefreshError{.message = error->message};
                }

                emailCount = parsedEmails.value->list.size();
                reportProgress(QStringLiteral("Cached %1 message summaries.").arg(emailCount));
            }
        }

        m_impl->statusSummary = QStringLiteral("Loaded %1 mailboxes and %2 messages from %3.")
                                    .arg(parsedMailboxes.value->list.size())
                                    .arg(emailCount)
                                    .arg(QString::fromStdString(settings.loginEmail));
        qInfo().noquote() << "JMAP core refresh success" << m_impl->statusSummary;

        co_return LiveRefreshSummary{
            .accountId = accountId,
            .selectedMailboxId = selectedMailboxId,
            .mailboxCount = parsedMailboxes.value->list.size(),
            .emailCount = emailCount,
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
        reportProgress(QStringLiteral("Loading message content..."));
        if (m_impl->databaseConnection == nullptr || m_impl->transport == nullptr)
        {
            co_return LiveRefreshError{
                .message =
                    QStringLiteral("Live refresh is unavailable in this process configuration."),
            };
        }

        if (settings.loginEmail.empty() || settings.apiKey.empty())
        {
            co_return LiveRefreshError{
                .message = QStringLiteral("Login email and API key are required."),
            };
        }

        javelin::jmap::cache::MessageContentRepository contentRepository{
            *m_impl->databaseConnection};
        javelin::jmap::cache::InlinePartPayloadRepository payloadRepository{
            *m_impl->databaseConnection};
        const auto cachedParts = contentRepository.loadParts(accountId, emailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&cachedParts))
        {
            co_return LiveRefreshError{.message = error->message};
        }

        const auto cachedBodyValues = contentRepository.loadBodyValues(accountId, emailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&cachedBodyValues))
        {
            co_return LiveRefreshError{.message = error->message};
        }

        const auto& parts = std::get<std::vector<javelin::jmap::cache::EmailPart>>(cachedParts);
        const auto& bodyValues =
            std::get<std::vector<javelin::jmap::cache::EmailBodyValue>>(cachedBodyValues);
        const auto cachedInlineMissing =
            missingInlineImageParts(parts, payloadRepository, accountId);
        if ((!parts.empty() || !bodyValues.empty()) && cachedInlineMissing.empty())
        {
            qInfo().noquote() << "JMAP core message content using cached data"
                              << QString::fromStdString(emailId);
            reportProgress(QStringLiteral("Loaded message content from local cache."));
            co_return MessageContentRefreshSummary{
                .accountId = std::move(accountId),
                .emailId = std::move(emailId),
                .partCount = parts.size(),
                .bodyValueCount = bodyValues.size(),
                .usedCachedContent = true,
            };
        }

        javelin::jmap::cache::SessionRepository sessionRepository{*m_impl->databaseConnection};
        const auto cachedSession = sessionRepository.load(accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&cachedSession))
        {
            co_return LiveRefreshError{.message = error->message};
        }

        const auto& session = std::get<std::optional<javelin::jmap::api::Session>>(cachedSession);
        if (!session.has_value())
        {
            co_return LiveRefreshError{
                .message = QStringLiteral("No cached session exists for the selected account yet."),
            };
        }

        javelin::jmap::api::MethodCaller methodCaller{*m_impl->transport};
        const javelin::jmap::api::ApiRequestContext apiRequestContext{
            .credentials =
                {
                    .accountId = accountId,
                    .emailAddress = settings.loginEmail,
                    .sessionUrl = settings.sessionUrl,
                    .token =
                        {
                            .accessToken = settings.apiKey,
                            .refreshToken = std::nullopt,
                            .expiry = std::nullopt,
                        },
                },
            .apiUrl = session->apiUrl,
        };

        const javelin::jmap::auth::AccountCredentials downloadCredentials{
            .accountId = accountId,
            .emailAddress = settings.loginEmail,
            .sessionUrl = settings.sessionUrl,
            .token =
                {
                    .accessToken = settings.apiKey,
                    .refreshToken = std::nullopt,
                    .expiry = std::nullopt,
                },
        };

        if (!cachedInlineMissing.empty())
        {
            if (const auto downloadError = co_await cacheInlineImageParts(
                    *m_impl->transport, downloadCredentials, session->downloadUrl, accountId,
                    cachedInlineMissing, payloadRepository))
            {
                co_return *downloadError;
            }

            qInfo().noquote() << "JMAP core inline payload refresh success from cached MIME"
                              << QString::fromStdString(emailId)
                              << static_cast<qulonglong>(cachedInlineMissing.size());
            reportProgress(QStringLiteral("Loaded message content from local cache."));
            co_return MessageContentRefreshSummary{
                .accountId = std::move(accountId),
                .emailId = std::move(emailId),
                .partCount = parts.size(),
                .bodyValueCount = bodyValues.size(),
                .usedCachedContent = true,
            };
        }

        const auto requestArguments = javelin::jmap::api::serializeEmailContentGetRequest({
            .accountId = accountId,
            .ids = {emailId},
            .properties = {"id", "textBody", "htmlBody", "attachments", "bodyValues"},
            .bodyProperties = {"partId", "blobId", "size", "name", "type", "charset", "disposition",
                               "cid"},
            .fetchTextBodyValues = true,
            .fetchHTMLBodyValues = true,
            .fetchAllBodyValues = false,
            .maxBodyValueBytes = 262144,
        });
        if (!requestArguments.has_value())
        {
            co_return LiveRefreshError{
                .message = QStringLiteral("Failed to encode the Email/get content request."),
            };
        }

        const javelin::jmap::api::RequestEnvelope request{
            .usingCapabilities =
                {
                    std::string{javelin::jmap::api::coreCapabilityUri},
                    std::string{javelin::jmap::api::mailCapabilityUri},
                },
            .methodCalls =
                {
                    javelin::jmap::api::MethodInvocation{
                        .name = "Email/get",
                        .arguments = *requestArguments,
                        .callId = "email-content",
                    },
                },
            .createdIds = std::nullopt,
        };

        const auto envelopeResult = co_await methodCaller.call(apiRequestContext, request);
        if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&envelopeResult))
        {
            co_return LiveRefreshError{.message = transportMessage(*error)};
        }
        if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&envelopeResult))
        {
            co_return LiveRefreshError{.message = authMessage(*error)};
        }
        if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&envelopeResult))
        {
            co_return LiveRefreshError{.message = protocolMessage(*error)};
        }

        const auto& envelope = std::get<javelin::jmap::api::ResponseEnvelope>(envelopeResult);
        const auto method = findMethodResponse(envelope, "Email/get", "email-content");
        if (!method.has_value())
        {
            co_return LiveRefreshError{
                .message = QStringLiteral(
                    "The server response did not contain Email/get content results."),
            };
        }

        const auto parsed = javelin::jmap::api::parseEmailContentGetResponse(method->arguments);
        if (!parsed.ok() || !parsed.value.has_value())
        {
            const auto dumped =
                dumpRawJsonToTempFile(QStringLiteral("email-content-get"), method->arguments);
            co_return LiveRefreshError{
                .message = dumped.has_value()
                               ? QStringLiteral("Failed to parse Email/get content response. Raw "
                                                "JSON dumped to %1.")
                                     .arg(*dumped)
                               : QStringLiteral("Failed to parse Email/get content response."),
            };
        }

        const auto contentIt =
            std::ranges::find(parsed.value->list, emailId, &javelin::jmap::api::EmailContent::id);
        if (contentIt == parsed.value->list.end())
        {
            co_return LiveRefreshError{
                .message = QStringLiteral("The selected message was not returned by Email/get."),
            };
        }

        const auto contentParts = buildContentParts(*contentIt);
        const auto contentBodyValues = buildBodyValues(*contentIt);
        if (const auto error = contentRepository.replaceForEmail(accountId, emailId, contentParts,
                                                                 contentBodyValues))
        {
            co_return LiveRefreshError{.message = error->message};
        }

        const auto missingInlineParts =
            missingInlineImageParts(contentParts, payloadRepository, accountId);
        if (!missingInlineParts.empty())
        {
            if (const auto downloadError = co_await cacheInlineImageParts(
                    *m_impl->transport, downloadCredentials, session->downloadUrl, accountId,
                    missingInlineParts, payloadRepository))
            {
                co_return *downloadError;
            }
        }

        qInfo().noquote() << "JMAP core message content refresh success"
                          << QString::fromStdString(emailId)
                          << static_cast<qulonglong>(contentParts.size())
                          << static_cast<qulonglong>(contentBodyValues.size());
        reportProgress(QStringLiteral("Fetched message body from server."));

        co_return MessageContentRefreshSummary{
            .accountId = std::move(accountId),
            .emailId = std::move(emailId),
            .partCount = contentParts.size(),
            .bodyValueCount = contentBodyValues.size(),
            .usedCachedContent = false,
        };
    }

    QCoro::Task<AttachmentDownloadResult>
    JmapCore::downloadAttachment(LiveConnectionSettings settings, std::string accountId,
                                 std::string emailId, std::string partId)
    {
        if (m_impl->databaseConnection == nullptr || m_impl->transport == nullptr)
        {
            co_return LiveRefreshError{
                .message = QStringLiteral(
                    "Attachment download is unavailable in this process configuration."),
            };
        }

        if (settings.sessionUrl.empty() || settings.loginEmail.empty() || settings.apiKey.empty())
        {
            co_return LiveRefreshError{
                .message = QStringLiteral("Session URL, login email, and API key are required."),
            };
        }

        javelin::jmap::cache::MessageContentRepository contentRepository{
            *m_impl->databaseConnection};
        QString loadErrorMessage;
        const auto part =
            findAttachmentPart(contentRepository, accountId, emailId, partId, loadErrorMessage);
        if (!part.has_value())
        {
            co_return LiveRefreshError{.message = loadErrorMessage};
        }

        javelin::jmap::cache::InlinePartPayloadRepository payloadRepository{
            *m_impl->databaseConnection};
        const auto payloadResult = payloadRepository.find(accountId, emailId, partId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&payloadResult))
        {
            co_return LiveRefreshError{.message = error->message};
        }

        const auto& cachedPayload =
            std::get<std::optional<javelin::jmap::cache::InlinePartPayload>>(payloadResult);
        if (cachedPayload.has_value() && cachedPayload->blobId == *part->blobId)
        {
            co_return AttachmentDownload{
                .accountId = std::move(accountId),
                .emailId = std::move(emailId),
                .partId = std::move(partId),
                .name = part->name,
                .mediaType = cachedPayload->mediaType,
                .payload = cachedPayload->payload,
                .usedCachedInlinePayload = true,
            };
        }

        javelin::jmap::cache::SessionRepository sessionRepository{*m_impl->databaseConnection};
        const auto sessionResult = sessionRepository.load(accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&sessionResult))
        {
            co_return LiveRefreshError{.message = error->message};
        }

        const auto& session = std::get<std::optional<javelin::jmap::api::Session>>(sessionResult);
        if (!session.has_value())
        {
            co_return LiveRefreshError{
                .message = QStringLiteral("No cached JMAP session is available for this account."),
            };
        }

        const javelin::jmap::auth::AccessTokenResolver accessTokenResolver;
        const auto tokenResult = accessTokenResolver.resolve({
            .accountId = accountId,
            .emailAddress = settings.loginEmail,
            .sessionUrl = settings.sessionUrl,
            .token =
                {
                    .accessToken = settings.apiKey,
                    .refreshToken = std::nullopt,
                    .expiry = std::nullopt,
                },
        });
        if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&tokenResult))
        {
            co_return LiveRefreshError{.message = authMessage(*error)};
        }

        const auto accessToken = std::get<javelin::jmap::auth::OAuthToken>(tokenResult).accessToken;
        const auto transportResult = co_await m_impl->transport->send(buildDownloadRequest(
            buildDownloadUrl(session->downloadUrl, accountId, *part), accessToken));
        if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&transportResult))
        {
            co_return LiveRefreshError{.message = transportMessage(*error)};
        }

        const auto& response = std::get<javelin::jmap::api::HttpResponse>(transportResult);
        if (response.statusCode < 200 || response.statusCode >= 300)
        {
            co_return LiveRefreshError{
                .message = QStringLiteral("Attachment download failed with HTTP status %1.")
                               .arg(response.statusCode),
            };
        }

        co_return AttachmentDownload{
            .accountId = std::move(accountId),
            .emailId = std::move(emailId),
            .partId = std::move(partId),
            .name = part->name,
            .mediaType = part->mediaType,
            .payload = response.body,
            .usedCachedInlinePayload = false,
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

        javelin::jmap::cache::SessionRepository sessionRepository{*m_impl->databaseConnection};
        const auto sessionResult = sessionRepository.load(accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&sessionResult))
        {
            co_return LiveRefreshError{.message = error->message};
        }

        const auto& session = std::get<std::optional<javelin::jmap::api::Session>>(sessionResult);
        if (!session.has_value())
        {
            co_return LiveRefreshError{
                .message = QStringLiteral("No cached session exists for the selected account yet."),
            };
        }

        javelin::jmap::api::MethodCaller methodCaller{*m_impl->transport};
        const javelin::jmap::api::ApiRequestContext apiRequestContext{
            .credentials =
                {
                    .accountId = accountId,
                    .emailAddress = settings.loginEmail,
                    .sessionUrl = settings.sessionUrl,
                    .token =
                        {
                            .accessToken = settings.apiKey,
                            .refreshToken = std::nullopt,
                            .expiry = std::nullopt,
                        },
                },
            .apiUrl = session->apiUrl,
        };

        const auto queryArguments = javelin::jmap::api::serializeEmailQueryRequest({
            .accountId = accountId,
            .filter =
                javelin::jmap::api::EmailQueryFilter{
                    .inMailbox = mailboxId,
                },
            .sort =
                {
                    javelin::jmap::api::EmailQuerySort{
                        .property = "receivedAt",
                        .isAscending = false,
                    },
                },
            .position = 0,
            .limit = 100,
            .collapseThreads = false,
            .calculateTotal = false,
        });
        if (!queryArguments.has_value())
        {
            co_return LiveRefreshError{
                .message = QStringLiteral("Failed to encode the mailbox Email/query request."),
            };
        }

        const javelin::jmap::api::RequestEnvelope queryRequest{
            .usingCapabilities =
                {
                    std::string{javelin::jmap::api::coreCapabilityUri},
                    std::string{javelin::jmap::api::mailCapabilityUri},
                },
            .methodCalls =
                {
                    javelin::jmap::api::MethodInvocation{
                        .name = "Email/query",
                        .arguments = *queryArguments,
                        .callId = "mailbox-query",
                    },
                },
            .createdIds = std::nullopt,
        };

        const auto queryEnvelopeResult =
            co_await methodCaller.call(apiRequestContext, queryRequest);
        if (const auto* error =
                std::get_if<javelin::jmap::api::TransportError>(&queryEnvelopeResult))
        {
            co_return LiveRefreshError{.message = transportMessage(*error)};
        }
        if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&queryEnvelopeResult))
        {
            co_return LiveRefreshError{.message = authMessage(*error)};
        }
        if (const auto* error =
                std::get_if<javelin::jmap::api::ProtocolError>(&queryEnvelopeResult))
        {
            co_return LiveRefreshError{.message = protocolMessage(*error)};
        }

        const auto& queryEnvelope =
            std::get<javelin::jmap::api::ResponseEnvelope>(queryEnvelopeResult);
        const auto queryMethod = findMethodResponse(queryEnvelope, "Email/query", "mailbox-query");
        if (!queryMethod.has_value())
        {
            co_return LiveRefreshError{
                .message =
                    QStringLiteral("The server response did not contain Email/query results."),
            };
        }

        const auto parsedQuery =
            javelin::jmap::api::parseEmailQueryResponse(queryMethod->arguments);
        if (!parsedQuery.ok() || !parsedQuery.value.has_value())
        {
            const auto dumped = dumpRawJsonToTempFile(QStringLiteral("mailbox-email-query"),
                                                      queryMethod->arguments);
            co_return LiveRefreshError{
                .message = dumped.has_value()
                               ? QStringLiteral("Failed to parse mailbox Email/query response. Raw "
                                                "JSON dumped to %1.")
                                     .arg(*dumped)
                               : QStringLiteral("Failed to parse mailbox Email/query response."),
            };
        }

        reportProgress(QStringLiteral("Fetched %1 message ids for the selected mailbox.")
                           .arg(parsedQuery.value->ids.size()));

        javelin::jmap::cache::SyncStateRepository syncStateRepository{*m_impl->databaseConnection};
        if (const auto error = syncStateRepository.upsert(
                {.accountId = accountId, .objectType = "EmailQuery", .queryKey = mailboxId},
                parsedQuery.value->queryState))
        {
            co_return LiveRefreshError{.message = error->message};
        }

        if (parsedQuery.value->ids.empty())
        {
            reportProgress(QStringLiteral("Selected mailbox has no messages."));
            co_return MailboxMessagesRefreshSummary{
                .accountId = std::move(accountId),
                .mailboxId = std::move(mailboxId),
                .emailCount = 0,
            };
        }

        reportProgress(QStringLiteral("Fetching message summaries for selected mailbox..."));
        const auto emailArguments = javelin::jmap::api::serializeGetRequest({
            .accountId = accountId,
            .ids = parsedQuery.value->ids,
            .properties = std::nullopt,
        });
        if (!emailArguments.has_value())
        {
            co_return LiveRefreshError{
                .message = QStringLiteral("Failed to encode the mailbox Email/get request."),
            };
        }

        const javelin::jmap::api::RequestEnvelope emailRequest{
            .usingCapabilities =
                {
                    std::string{javelin::jmap::api::coreCapabilityUri},
                    std::string{javelin::jmap::api::mailCapabilityUri},
                },
            .methodCalls =
                {
                    javelin::jmap::api::MethodInvocation{
                        .name = "Email/get",
                        .arguments = *emailArguments,
                        .callId = "mailbox-emails-get",
                    },
                },
            .createdIds = std::nullopt,
        };

        const auto emailEnvelopeResult =
            co_await methodCaller.call(apiRequestContext, emailRequest);
        if (const auto* error =
                std::get_if<javelin::jmap::api::TransportError>(&emailEnvelopeResult))
        {
            co_return LiveRefreshError{.message = transportMessage(*error)};
        }
        if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&emailEnvelopeResult))
        {
            co_return LiveRefreshError{.message = authMessage(*error)};
        }
        if (const auto* error =
                std::get_if<javelin::jmap::api::ProtocolError>(&emailEnvelopeResult))
        {
            co_return LiveRefreshError{.message = protocolMessage(*error)};
        }

        const auto& emailEnvelope =
            std::get<javelin::jmap::api::ResponseEnvelope>(emailEnvelopeResult);
        const auto emailMethod =
            findMethodResponse(emailEnvelope, "Email/get", "mailbox-emails-get");
        if (!emailMethod.has_value())
        {
            co_return LiveRefreshError{
                .message = QStringLiteral("The server response did not contain Email/get results."),
            };
        }

        const auto parsedEmails = javelin::jmap::api::parseEmailGetResponse(emailMethod->arguments);
        if (!parsedEmails.ok() || !parsedEmails.value.has_value())
        {
            const auto dumped =
                dumpRawJsonToTempFile(QStringLiteral("mailbox-email-get"), emailMethod->arguments);
            co_return LiveRefreshError{
                .message =
                    dumped.has_value()
                        ? QStringLiteral(
                              "Failed to parse mailbox Email/get response. Raw JSON dumped to %1.")
                              .arg(*dumped)
                        : QStringLiteral("Failed to parse mailbox Email/get response."),
            };
        }

        javelin::jmap::cache::EmailRepository emailRepository{*m_impl->databaseConnection};
        if (const auto error = emailRepository.upsertMany(accountId, parsedEmails.value->list))
        {
            co_return LiveRefreshError{.message = error->message};
        }

        if (const auto error = syncStateRepository.upsert(
                {.accountId = accountId, .objectType = "Email", .queryKey = mailboxId},
                parsedEmails.value->state))
        {
            co_return LiveRefreshError{.message = error->message};
        }

        reportProgress(QStringLiteral("Cached %1 messages for the selected mailbox.")
                           .arg(parsedEmails.value->list.size()));
        co_return MailboxMessagesRefreshSummary{
            .accountId = std::move(accountId),
            .mailboxId = std::move(mailboxId),
            .emailCount = parsedEmails.value->list.size(),
        };
    }

} // namespace javelin::jmap
