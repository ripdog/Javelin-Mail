#include "jmap/submission/ComposeService.h"

#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MailMethods.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/PatchObject.h"
#include "jmap/api/ResponseReader.h"
#include "jmap/api/Session.h"
#include "jmap/api/Transport.h"
#include "jmap/auth/AccessTokenResolver.h"
#include "jmap/cache/ComposeSessionRepository.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/IdentityRepository.h"
#include "jmap/cache/MailVault.h"
#include "jmap/cache/MessageViewService.h"
#include "jmap/cache/QueryService.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/SubmissionRepository.h"
#include "jmap/render/HtmlBodyEmbedding.h"
#include "jmap/render/HtmlTextExtractor.h"
#include "jmap/submission/DraftInlineImageStorage.h"
#include "jmap/submission/DraftMutationJournal.h"
#include "jmap/sync/ConsistencyDomain.h"
#include "jmap/sync/EmailMutationJournal.h"
#include "jmap/sync/MutationJournal.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QString>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <unordered_set>

namespace javelin::jmap::submission
{

    namespace
    {

        [[nodiscard]] std::optional<javelin::jmap::OperationError>
        validateSettings(const javelin::jmap::LiveConnectionSettings& settings)
        {
            if (settings.sessionUrl.empty() || settings.loginEmail.empty() ||
                settings.apiKey.empty())
            {
                return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                    .message =
                        QStringLiteral("Session URL, login email, and API key are required."),
                };
            }

            return std::nullopt;
        }

        [[nodiscard]] javelin::jmap::auth::AccountCredentials
        buildCredentials(const javelin::jmap::LiveConnectionSettings& settings,
                         std::string accountId)
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

        [[nodiscard]] std::variant<javelin::jmap::api::Session, javelin::jmap::OperationError>
        loadCachedSession(javelin::jmap::cache::DatabaseConnection& connection,
                          const std::string_view accountId)
        {
            javelin::jmap::cache::SessionRepository sessionRepository{connection};
            const auto result = sessionRepository.load(accountId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            {
                return javelin::jmap::operationError(*error);
            }

            const auto& session = std::get<std::optional<javelin::jmap::api::Session>>(result);
            if (!session.has_value())
            {
                return javelin::jmap::OperationError{
                    .message =
                        QStringLiteral("No cached JMAP session is available for this account."),
                };
            }

            return *session;
        }

        [[nodiscard]] std::variant<std::string, javelin::jmap::OperationError>
        resolveAccessToken(const javelin::jmap::auth::AccountCredentials& credentials)
        {
            const javelin::jmap::auth::AccessTokenResolver resolver;
            const auto result = resolver.resolve(credentials);
            if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&result))
            {
                return javelin::jmap::operationError(*error);
            }

            return std::get<javelin::jmap::auth::OAuthToken>(result).accessToken;
        }

        [[nodiscard]] javelin::jmap::api::ApiRequestContext
        buildApiRequestContext(const javelin::jmap::LiveConnectionSettings& settings,
                               std::string accountId, const javelin::jmap::api::Session& session)
        {
            return javelin::jmap::api::ApiRequestContext{
                .credentials = buildCredentials(settings, std::move(accountId)),
                .apiUrl = session.apiUrl,
                .requestLimits = javelin::jmap::api::coreRequestLimits(session),
            };
        }

        [[nodiscard]] std::optional<javelin::jmap::cache::MailboxTreeItem>
        findMailboxByRole(javelin::jmap::cache::DatabaseConnection& connection,
                          const std::string_view accountId, const std::string_view role)
        {
            javelin::jmap::cache::QueryService queryService{connection};
            const auto result = queryService.listMailboxTree(accountId);
            const auto* mailboxes =
                std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&result);
            if (mailboxes == nullptr)
            {
                return std::nullopt;
            }

            for (const auto& mailbox : *mailboxes)
            {
                if (mailbox.role == std::optional<std::string>{std::string{role}})
                {
                    return mailbox;
                }
            }

            return std::nullopt;
        }

        [[nodiscard]] std::string nowIsoUtc()
        {
            return QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
        }

        [[nodiscard]] std::string trimSubjectPrefix(const std::string_view subject,
                                                    const std::string_view prefix)
        {
            const auto subjectString = QString::fromStdString(std::string{subject});
            if (subjectString.startsWith(QString::fromStdString(std::string{prefix}),
                                         Qt::CaseInsensitive))
            {
                return subjectString.toStdString();
            }

            return QString(QString::fromStdString(std::string{prefix}) + QStringLiteral(" ") +
                           subjectString)
                .toStdString();
        }

        [[nodiscard]] std::string addressLabel(const javelin::jmap::domain::EmailAddress& address)
        {
            if (address.name.has_value() && !address.name->empty())
            {
                return *address.name + " <" + address.email + ">";
            }

            return address.email;
        }

        [[nodiscard]] std::string
        joinAddresses(const std::vector<javelin::jmap::domain::EmailAddress>& addresses)
        {
            QStringList parts;
            for (const auto& address : addresses)
            {
                parts.push_back(QString::fromStdString(addressLabel(address)));
            }
            return parts.join(QStringLiteral(", ")).toStdString();
        }

        [[nodiscard]] std::string buildReplyPlainText(const javelin::jmap::domain::Email& email,
                                                      const std::string_view body)
        {
            QStringList lines;
            lines << QString{};
            lines << QString::fromStdString(
                QStringLiteral("On %1, %2 wrote:")
                    .arg(QString::fromStdString(email.sentAt.value_or(email.receivedAt)),
                         QString::fromStdString(joinAddresses(email.from)))
                    .toStdString());

            const auto text = QString::fromStdString(std::string{body});
            const auto bodyLines = text.split(QLatin1Char('\n'));
            for (const auto& line : bodyLines)
            {
                lines << (QStringLiteral("> ") + line);
            }
            return lines.join(QLatin1Char('\n')).toStdString();
        }

        [[nodiscard]] std::string buildReplyHtml(const javelin::jmap::domain::Email& email,
                                                 const std::string_view htmlBody)
        {
            const auto sentAt =
                QString::fromStdString(email.sentAt.value_or(email.receivedAt)).toHtmlEscaped();
            const auto from = QString::fromStdString(joinAddresses(email.from)).toHtmlEscaped();
            const auto cite =
                email.messageId.empty()
                    ? QString{}
                    : QStringLiteral(" cite=\"%1\"")
                          .arg(QStringLiteral("mid:%1")
                                   .arg(QString::fromStdString(email.messageId.front()))
                                   .toHtmlEscaped());
            const auto embeddedBody = javelin::jmap::render::htmlBodyContentForEmbedding(
                QString::fromStdString(std::string{htmlBody}));
            return QStringLiteral("<p><br/></p><div class=\"moz-cite-prefix\">On %1, %2 "
                                  "wrote:<br/></div><blockquote type=\"cite\"%3>%4</blockquote>")
                .arg(sentAt, from, cite, embeddedBody)
                .toStdString();
        }

        [[nodiscard]] std::string buildForwardPlainText(const javelin::jmap::domain::Email& email,
                                                        const std::string_view body)
        {
            return QStringLiteral(
                       "\n\n---------- Forwarded message ----------\nFrom: %1\nTo: %2\nSubject: "
                       "%3\nDate: %4\n\n%5")
                .arg(QString::fromStdString(joinAddresses(email.from)),
                     QString::fromStdString(joinAddresses(email.to)),
                     QString::fromStdString(email.subject.value_or(std::string{})),
                     QString::fromStdString(email.sentAt.value_or(email.receivedAt)),
                     QString::fromStdString(std::string{body}))
                .toStdString();
        }

        [[nodiscard]] std::string buildForwardHtml(const javelin::jmap::domain::Email& email,
                                                   const std::string_view htmlBody)
        {
            const auto embeddedBody = javelin::jmap::render::htmlBodyContentForEmbedding(
                QString::fromStdString(std::string{htmlBody}));
            return QStringLiteral(
                       "<p><br/></p><hr/><p><b>From:</b> %1<br/><b>To:</b> %2<br/><b>Subject:</b> "
                       "%3<br/><b>Date:</b> %4</p>%5")
                .arg(QString::fromStdString(joinAddresses(email.from)),
                     QString::fromStdString(joinAddresses(email.to)),
                     QString::fromStdString(email.subject.value_or(std::string{})),
                     QString::fromStdString(email.sentAt.value_or(email.receivedAt)), embeddedBody)
                .toStdString();
        }

        [[nodiscard]] std::vector<javelin::jmap::domain::EmailAddress>
        deduplicateAddresses(const std::vector<javelin::jmap::domain::EmailAddress>& first,
                             const std::vector<javelin::jmap::domain::EmailAddress>& second,
                             const std::unordered_set<std::string>& excludedEmails)
        {
            std::vector<javelin::jmap::domain::EmailAddress> addresses;
            std::unordered_set<std::string> seen = excludedEmails;
            const auto appendUnique =
                [&addresses, &seen](const std::vector<javelin::jmap::domain::EmailAddress>& source)
            {
                for (const auto& address : source)
                {
                    if (address.email.empty())
                    {
                        continue;
                    }

                    auto normalized = address.email;
                    std::ranges::transform(normalized, normalized.begin(), [](unsigned char c)
                                           { return static_cast<char>(std::tolower(c)); });
                    if (!seen.insert(normalized).second)
                    {
                        continue;
                    }
                    addresses.push_back(address);
                }
            };

            appendUnique(first);
            appendUnique(second);
            return addresses;
        }

        [[nodiscard]] std::string strippedPlainText(const std::string_view html)
        {
            return javelin::jmap::render::plainTextFromHtml(
                       QString::fromStdString(std::string{html}))
                .toStdString();
        }

        [[nodiscard]] std::string htmlFromText(const std::string_view plainText)
        {
            QStringList paragraphs;
            const auto lines = QString::fromStdString(std::string{plainText})
                                   .split(QLatin1Char('\n'), Qt::KeepEmptyParts);
            paragraphs.reserve(lines.size());
            for (const auto& line : lines)
            {
                paragraphs.push_back(line.isEmpty()
                                         ? QStringLiteral("<p>&nbsp;</p>")
                                         : QStringLiteral("<p>%1</p>").arg(line.toHtmlEscaped()));
            }
            return paragraphs.join(QLatin1Char('\n')).toStdString();
        }

        [[nodiscard]] std::vector<DraftAttachment> draftAttachmentsFromMessage(
            const std::vector<javelin::jmap::cache::MessageAttachment>& attachments)
        {
            std::vector<DraftAttachment> draftAttachments;
            draftAttachments.reserve(attachments.size());
            for (const auto& attachment : attachments)
            {
                draftAttachments.push_back(DraftAttachment{
                    .localFilePath = {},
                    .displayName = attachment.name.value_or(std::string{}),
                    .mediaType = attachment.mediaType,
                    .size = attachment.size,
                    .blobId = attachment.blobId,
                    .inlineDisposition =
                        attachment.disposition == std::optional<std::string>{std::string{"inline"}},
                    .contentId = attachment.cid,
                    .contentHash = std::nullopt,
                });
            }
            return draftAttachments;
        }

        [[nodiscard]] QCoro::Task<
            std::variant<std::vector<DraftAttachment>, javelin::jmap::OperationError>>
        materializeInlineDraftImages(
            javelin::jmap::cache::DatabaseConnection& connection, javelin::jmap::JmapCore& core,
            javelin::jmap::LiveConnectionSettings settings, const std::string& accountId,
            const std::string& emailId,
            const std::vector<javelin::jmap::cache::MessageAttachment>& sourceAttachments)
        {
            auto draftAttachments = draftAttachmentsFromMessage(sourceAttachments);
            const auto vault = javelin::jmap::cache::MailVault::forDatabase(connection);
            for (std::size_t index = 0; index < sourceAttachments.size(); ++index)
            {
                const auto& source = sourceAttachments[index];
                auto& draft = draftAttachments[index];
                if (!draft.inlineDisposition || !draft.contentId.has_value() ||
                    !source.mediaType.starts_with("image/") || source.partId.empty())
                {
                    continue;
                }

                const auto download =
                    co_await core.downloadAttachment(settings, accountId, emailId, source.partId);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&download))
                {
                    co_return *error;
                }
                auto materialized = materializeDraftInlineImage(
                    vault, std::move(draft), std::get<javelin::jmap::AttachmentDownload>(download));
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&materialized))
                {
                    co_return *error;
                }
                draft = std::get<DraftAttachment>(std::move(materialized));
            }
            co_return draftAttachments;
        }

        [[nodiscard]] bool isWildcardSenderIdentity(const javelin::jmap::domain::Identity& identity)
        {
            return identity.email.starts_with("*@");
        }

        [[nodiscard]] std::vector<javelin::jmap::domain::Identity>
        senderIdentities(const std::vector<javelin::jmap::domain::Identity>& identities)
        {
            std::vector<javelin::jmap::domain::Identity> filtered;
            filtered.reserve(identities.size());
            std::copy_if(identities.cbegin(), identities.cend(), std::back_inserter(filtered),
                         [](const auto& identity) { return !isWildcardSenderIdentity(identity); });
            return filtered;
        }

        [[nodiscard]] std::optional<javelin::jmap::domain::Identity> chooseDefaultIdentity(
            const std::vector<javelin::jmap::domain::Identity>& identities,
            const std::optional<std::vector<javelin::jmap::domain::EmailAddress>>& fromAddresses =
                std::nullopt)
        {
            if (identities.empty())
            {
                return std::nullopt;
            }

            if (fromAddresses.has_value())
            {
                for (const auto& fromAddress : *fromAddresses)
                {
                    const auto it = std::find_if(identities.cbegin(), identities.cend(),
                                                 [&fromAddress](const auto& identity)
                                                 {
                                                     return !isWildcardSenderIdentity(identity) &&
                                                            identity.email == fromAddress.email;
                                                 });
                    if (it != identities.cend())
                    {
                        return *it;
                    }
                }
            }

            const auto it =
                std::find_if(identities.cbegin(), identities.cend(), [](const auto& identity)
                             { return !isWildcardSenderIdentity(identity); });
            return it == identities.cend() ? std::nullopt : std::optional{*it};
        }

        [[nodiscard]] QCoro::Task<std::variant<std::vector<javelin::jmap::domain::Identity>,
                                               javelin::jmap::OperationError>>
        ensureIdentities(javelin::jmap::cache::DatabaseConnection& connection,
                         javelin::jmap::api::JmapMethodTransport& methodTransport,
                         javelin::jmap::LiveConnectionSettings settings, std::string accountId)
        {
            javelin::jmap::cache::IdentityRepository identityRepository{connection};
            const auto cachedResult = identityRepository.listByAccount(accountId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&cachedResult))
            {
                co_return javelin::jmap::operationError(*error);
            }

            const auto& cached =
                std::get<std::vector<javelin::jmap::domain::Identity>>(cachedResult);
            if (!cached.empty())
            {
                co_return cached;
            }

            const auto sessionResult = loadCachedSession(connection, accountId);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&sessionResult))
            {
                co_return *error;
            }
            const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);

            if (!session.capabilities.submission)
            {
                co_return javelin::jmap::OperationError{
                    .message =
                        QStringLiteral("This account does not advertise JMAP submission support."),
                };
            }

            javelin::jmap::api::MethodCaller methodCaller{methodTransport};
            javelin::jmap::api::RequestBuilder builder;
            builder.useCore().useCapability(
                std::string{javelin::jmap::api::submissionCapabilityUri});
            const auto request = javelin::jmap::api::identityGet({.accountId = accountId,
                                                                  .ids = std::nullopt,
                                                                  .idsReference = std::nullopt,
                                                                  .properties = std::nullopt});
            if (!request.has_value())
            {
                co_return javelin::jmap::OperationError{
                    .message = QStringLiteral("Failed to encode the Identity/get request."),
                };
            }
            const auto handle = builder.call(*request, "identities");
            const auto envelopeResult = co_await methodCaller.call(
                buildApiRequestContext(settings, accountId, session), builder);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::TransportError>(&envelopeResult))
            {
                co_return javelin::jmap::operationError(*error);
            }
            if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&envelopeResult))
            {
                co_return javelin::jmap::operationError(*error);
            }
            if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&envelopeResult))
            {
                co_return javelin::jmap::operationError(*error);
            }

            const auto& envelope = std::get<javelin::jmap::api::ResponseEnvelope>(envelopeResult);
            const javelin::jmap::api::ResponseReader reader{envelope};
            const auto identityResult = reader.require(handle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&identityResult))
            {
                const auto rawResponses = reader.rawAll(handle.callId);
                for (const auto& response : rawResponses)
                {
                    qWarning().noquote()
                        << "JMAP Identity/get failed"
                        << "method" << QString::fromStdString(response.name) << "callId"
                        << QString::fromStdString(response.callId) << "arguments"
                        << QString::fromStdString(response.arguments);
                }
                co_return javelin::jmap::operationError(*error);
            }

            const auto& identities =
                std::get<javelin::jmap::api::IdentityGetResponse>(identityResult).list;
            if (const auto error = identityRepository.replaceAll(accountId, identities))
            {
                co_return javelin::jmap::operationError(*error);
            }

            co_return identities;
        }

        [[nodiscard]] std::string uploadUrlForAccount(const std::string_view templateUrl,
                                                      const std::string_view accountId)
        {
            QString expanded = QString::fromStdString(std::string{templateUrl});
            expanded.replace(QStringLiteral("{accountId}"),
                             QString::fromUtf8(QUrl::toPercentEncoding(
                                 QString::fromStdString(std::string{accountId}))));
            return expanded.toStdString();
        }

        struct UploadSummary
        {
            std::string accountId;
            std::string blobId;
            std::string type;
            std::uint64_t size = 0;
            std::string contentHash;
        };

        [[nodiscard]] QCoro::Task<std::variant<UploadSummary, javelin::jmap::OperationError>>
        uploadAttachment(javelin::jmap::api::AbstractTransport& transport,
                         javelin::jmap::LiveConnectionSettings settings,
                         javelin::jmap::api::Session session, std::string accountId,
                         DraftAttachment attachment, QByteArray body)
        {
            const auto tokenResult = resolveAccessToken(buildCredentials(settings, accountId));
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&tokenResult))
            {
                co_return *error;
            }

            const auto transportResult = co_await transport.send({
                .method = javelin::jmap::api::HttpMethod::Post,
                .url =
                    QUrl{QString::fromStdString(uploadUrlForAccount(session.uploadUrl, accountId))},
                .headers =
                    {
                        javelin::jmap::api::HttpHeader{
                            .name = "Authorization",
                            .value = QByteArray{"Bearer "} +
                                     QByteArray::fromStdString(std::get<std::string>(tokenResult)),
                        },
                        javelin::jmap::api::HttpHeader{
                            .name = "Accept",
                            .value = "application/json",
                        },
                        javelin::jmap::api::HttpHeader{
                            .name = "Content-Type",
                            .value = QByteArray::fromStdString(attachment.mediaType),
                        },
                    },
                .body = body,
                .authentication =
                    javelin::jmap::api::BearerAuthentication{
                        .accountId = accountId,
                        .accessToken = std::get<std::string>(tokenResult),
                    },
                .cancellation = {},
                .dispatched = {},
            });
            if (const auto* error =
                    std::get_if<javelin::jmap::api::TransportError>(&transportResult))
            {
                co_return javelin::jmap::operationError(*error);
            }

            const auto response = std::get<javelin::jmap::api::HttpResponse>(transportResult);
            const auto document = QJsonDocument::fromJson(response.body);
            if (!document.isObject())
            {
                co_return javelin::jmap::OperationError{
                    .message = QStringLiteral("Failed to decode the attachment upload response."),
                };
            }

            const auto object = document.object();
            const auto blobId = object.value(QStringLiteral("blobId")).toString();
            if (blobId.isEmpty())
            {
                co_return javelin::jmap::OperationError{
                    .message = QStringLiteral("The upload response did not contain a blob id."),
                };
            }

            co_return UploadSummary{
                .accountId = object.value(QStringLiteral("accountId"))
                                 .toString(QString::fromStdString(accountId))
                                 .toStdString(),
                .blobId = blobId.toStdString(),
                .type = object.value(QStringLiteral("type"))
                            .toString(QString::fromStdString(attachment.mediaType))
                            .toStdString(),
                .size = static_cast<std::uint64_t>(
                    object.value(QStringLiteral("size"))
                        .toInteger(static_cast<qint64>(attachment.size))),
                .contentHash =
                    QString::fromLatin1(
                        QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex())
                        .toStdString(),
            };
        }

        [[nodiscard]] javelin::jmap::api::EmailBodyPartCreate
        buildBodyStructure(const DraftSnapshot& snapshot)
        {
            const bool hasPlain = !snapshot.plainTextBody.empty();
            const bool hasHtml = !snapshot.htmlBody.empty();

            std::vector<javelin::jmap::api::EmailBodyPartCreate> bodyParts;
            if (hasHtml && hasPlain)
            {
                bodyParts.push_back(javelin::jmap::api::EmailBodyPartCreate{
                    .partId = std::string{"text-body"},
                    .blobId = std::nullopt,
                    .type = "text/plain",
                    .name = std::nullopt,
                    .disposition = std::nullopt,
                    .cid = std::nullopt,
                    .subParts = std::nullopt,
                });
                bodyParts.push_back(javelin::jmap::api::EmailBodyPartCreate{
                    .partId = std::string{"html-body"},
                    .blobId = std::nullopt,
                    .type = "text/html",
                    .name = std::nullopt,
                    .disposition = std::nullopt,
                    .cid = std::nullopt,
                    .subParts = std::nullopt,
                });
            }
            else if (hasHtml)
            {
                bodyParts.push_back(javelin::jmap::api::EmailBodyPartCreate{
                    .partId = std::string{"html-body"},
                    .blobId = std::nullopt,
                    .type = "text/html",
                    .name = std::nullopt,
                    .disposition = std::nullopt,
                    .cid = std::nullopt,
                    .subParts = std::nullopt,
                });
            }
            else
            {
                bodyParts.push_back(javelin::jmap::api::EmailBodyPartCreate{
                    .partId = std::string{"text-body"},
                    .blobId = std::nullopt,
                    .type = "text/plain",
                    .name = std::nullopt,
                    .disposition = std::nullopt,
                    .cid = std::nullopt,
                    .subParts = std::nullopt,
                });
            }

            javelin::jmap::api::EmailBodyPartCreate bodyRoot;
            if (bodyParts.size() == 1)
            {
                bodyRoot = bodyParts.front();
            }
            else
            {
                bodyRoot = javelin::jmap::api::EmailBodyPartCreate{
                    .partId = std::nullopt,
                    .blobId = std::nullopt,
                    .type = "multipart/alternative",
                    .name = std::nullopt,
                    .disposition = std::nullopt,
                    .cid = std::nullopt,
                    .subParts = bodyParts,
                };
            }

            std::vector<javelin::jmap::api::EmailBodyPartCreate> inlineParts;
            std::vector<javelin::jmap::api::EmailBodyPartCreate> attachmentParts;
            for (const auto& attachment : snapshot.attachments)
            {
                const bool inlineDisposition = !snapshot.htmlBody.empty() &&
                                               attachment.inlineDisposition &&
                                               attachment.contentId.has_value();
                auto part = javelin::jmap::api::EmailBodyPartCreate{
                    .partId = std::nullopt,
                    .blobId = attachment.blobId,
                    .type = attachment.mediaType,
                    .name = attachment.displayName.empty()
                                ? std::nullopt
                                : std::optional<std::string>{attachment.displayName},
                    .disposition = inlineDisposition ? std::optional<std::string>{"inline"}
                                                     : std::optional<std::string>{"attachment"},
                    .cid = inlineDisposition ? attachment.contentId : std::nullopt,
                    .subParts = std::nullopt,
                };
                if (inlineDisposition)
                {
                    inlineParts.push_back(std::move(part));
                }
                else
                {
                    attachmentParts.push_back(std::move(part));
                }
            }

            if (!inlineParts.empty())
            {
                std::vector<javelin::jmap::api::EmailBodyPartCreate> relatedParts;
                relatedParts.push_back(std::move(bodyRoot));
                relatedParts.insert(relatedParts.end(),
                                    std::make_move_iterator(inlineParts.begin()),
                                    std::make_move_iterator(inlineParts.end()));
                bodyRoot = javelin::jmap::api::EmailBodyPartCreate{
                    .partId = std::nullopt,
                    .blobId = std::nullopt,
                    .type = "multipart/related",
                    .name = std::nullopt,
                    .disposition = std::nullopt,
                    .cid = std::nullopt,
                    .subParts = std::move(relatedParts),
                };
            }

            if (attachmentParts.empty())
            {
                return bodyRoot;
            }

            std::vector<javelin::jmap::api::EmailBodyPartCreate> mixedParts;
            mixedParts.push_back(std::move(bodyRoot));
            mixedParts.insert(mixedParts.end(), std::make_move_iterator(attachmentParts.begin()),
                              std::make_move_iterator(attachmentParts.end()));

            return javelin::jmap::api::EmailBodyPartCreate{
                .partId = std::nullopt,
                .blobId = std::nullopt,
                .type = "multipart/mixed",
                .name = std::nullopt,
                .disposition = std::nullopt,
                .cid = std::nullopt,
                .subParts = std::move(mixedParts),
            };
        }

        [[nodiscard]] std::unordered_map<std::string, javelin::jmap::api::EmailBodyValueCreate>
        buildBodyValues(const DraftSnapshot& snapshot)
        {
            std::unordered_map<std::string, javelin::jmap::api::EmailBodyValueCreate> bodyValues;
            if (!snapshot.htmlBody.empty())
            {
                bodyValues.emplace("html-body", javelin::jmap::api::EmailBodyValueCreate{
                                                    .value = snapshot.htmlBody,
                                                    .isTruncated = false,
                                                });
            }
            if (!snapshot.plainTextBody.empty())
            {
                bodyValues.emplace("text-body", javelin::jmap::api::EmailBodyValueCreate{
                                                    .value = snapshot.plainTextBody,
                                                    .isTruncated = false,
                                                });
            }
            return bodyValues;
        }

        [[nodiscard]] javelin::jmap::domain::Email emailFromDraft(
            const DraftSnapshot& snapshot, const javelin::jmap::domain::Identity& identity,
            const std::string& draftMailboxId, const javelin::jmap::api::EmailSetCreated& created)
        {
            return javelin::jmap::domain::Email{
                .id = created.id,
                .blobId = created.blobId,
                .threadId = created.threadId,
                .mailboxIds = {draftMailboxId},
                .keywords = {"$seen", "$draft"},
                .size = created.size,
                .receivedAt = nowIsoUtc(),
                .sentAt = std::optional<std::string>{nowIsoUtc()},
                .messageId = snapshot.threading.messageId,
                .inReplyTo = snapshot.threading.inReplyTo,
                .references = snapshot.threading.references,
                .hasAttachment = !snapshot.attachments.empty(),
                .subject = snapshot.subject,
                .from = {javelin::jmap::domain::EmailAddress{
                    .name = identity.name.empty() ? std::nullopt
                                                  : std::optional<std::string>{identity.name},
                    .email = identity.email,
                }},
                .to = snapshot.to,
                .cc = snapshot.cc,
                .bcc = snapshot.bcc,
                .replyTo = identity.replyTo,
                .preview = snapshot.plainTextBody.empty()
                               ? std::nullopt
                               : std::optional<std::string>{snapshot.plainTextBody.substr(
                                     0, std::min<std::size_t>(snapshot.plainTextBody.size(), 160))},
            };
        }

        [[nodiscard]] std::string attachmentMediaType(const QString& path)
        {
            QMimeDatabase mimeDatabase;
            const auto mimeType = mimeDatabase.mimeTypeForFile(path, QMimeDatabase::MatchContent);
            return mimeType.isValid() ? mimeType.name().toStdString()
                                      : std::string{"application/octet-stream"};
        }

    } // namespace

    ComposeService::ComposeService(javelin::jmap::cache::DatabaseConnection& connection,
                                   javelin::jmap::api::AbstractTransport& resourceTransport,
                                   javelin::jmap::api::JmapMethodTransport& methodTransport,
                                   javelin::jmap::JmapCore& jmapCore)
        : m_connection(connection), m_resourceTransport(resourceTransport),
          m_methodTransport(methodTransport), m_jmapCore(jmapCore)
    {
    }

    QCoro::Task<
        std::variant<std::vector<javelin::jmap::domain::Identity>, javelin::jmap::OperationError>>
    ComposeService::loadSenderIdentities(javelin::jmap::LiveConnectionSettings settings,
                                         std::string accountId)
    {
        if (const auto validationError = validateSettings(settings))
        {
            co_return *validationError;
        }

        const auto identitiesResult = co_await ensureIdentities(
            m_connection, m_methodTransport, std::move(settings), std::move(accountId));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&identitiesResult))
        {
            co_return *error;
        }

        co_return senderIdentities(
            std::get<std::vector<javelin::jmap::domain::Identity>>(identitiesResult));
    }

    QCoro::Task<std::variant<DraftSnapshot, javelin::jmap::OperationError>>
    ComposeService::open(javelin::jmap::LiveConnectionSettings settings, OpenComposeRequest request)
    {
        if (const auto validationError = validateSettings(settings))
        {
            co_return *validationError;
        }

        javelin::jmap::cache::ComposeSessionRepository composeRepository{m_connection};
        if (request.draftEmailId.has_value() && request.useExistingWorkingCopy)
        {
            const auto composeSessionsResult = composeRepository.listByAccount(request.accountId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&composeSessionsResult))
            {
                co_return javelin::jmap::operationError(*error);
            }

            const auto& sessions = std::get<std::vector<DraftSnapshot>>(composeSessionsResult);
            const auto existing =
                std::find_if(sessions.cbegin(), sessions.cend(), [&request](const auto& snapshot)
                             { return snapshot.draftEmailId == request.draftEmailId; });
            if (existing != sessions.cend())
            {
                co_return *existing;
            }
        }

        const auto identitiesResult =
            co_await ensureIdentities(m_connection, m_methodTransport, settings, request.accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&identitiesResult))
        {
            co_return *error;
        }
        const auto& identities =
            std::get<std::vector<javelin::jmap::domain::Identity>>(identitiesResult);
        const auto availableSenderIdentities = senderIdentities(identities);
        if (availableSenderIdentities.empty())
        {
            co_return javelin::jmap::OperationError{
                .message = QStringLiteral("No sender identities are available for this account."),
            };
        }

        DraftSnapshot snapshot{
            .composeSessionId = request.composeSessionId.value_or(
                QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString()),
            .accountId = request.accountId,
            .draftEmailId = std::nullopt,
            .mode = request.mode,
            .editorMode = request.initialEditorMode,
            .identityId = availableSenderIdentities.front().id,
            .to = std::move(request.initialTo),
            .cc = {},
            .bcc = availableSenderIdentities.front().bcc,
            .subject = std::nullopt,
            .plainTextBody =
                availableSenderIdentities.front().textSignature.value_or(std::string{}),
            .htmlBody = availableSenderIdentities.front().htmlSignature.value_or(
                availableSenderIdentities.front().textSignature.has_value()
                    ? htmlFromText(*availableSenderIdentities.front().textSignature)
                    : std::string{}),
            .threading = {},
            .attachments = {},
        };

        if (request.mode == ComposeMode::NewMessage)
        {
            if (const auto error = composeRepository.upsert(snapshot))
            {
                co_return javelin::jmap::operationError(*error);
            }
            co_return snapshot;
        }

        const auto sourceEmailId =
            request.draftEmailId.has_value() ? request.draftEmailId : request.referenceEmailId;
        if (!sourceEmailId.has_value())
        {
            co_return javelin::jmap::OperationError{
                .message = QStringLiteral("A source message id is required."),
            };
        }

        const auto refreshResult =
            co_await m_jmapCore.refreshMessageContent(settings, request.accountId, *sourceEmailId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&refreshResult))
        {
            co_return *error;
        }

        javelin::jmap::cache::MessageViewService messageViewService{m_connection};
        const auto messageResult = messageViewService.load(request.accountId, *sourceEmailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&messageResult))
        {
            co_return javelin::jmap::operationError(*error);
        }

        const auto& messageSnapshot =
            std::get<std::optional<javelin::jmap::cache::MessageViewSnapshot>>(messageResult);
        if (!messageSnapshot.has_value())
        {
            co_return javelin::jmap::OperationError{
                .message = QStringLiteral("The selected message is unavailable."),
            };
        }

        const auto selectedIdentity =
            chooseDefaultIdentity(identities, std::optional{messageSnapshot->email.from});
        if (selectedIdentity.has_value())
        {
            snapshot.identityId = selectedIdentity->id;
            snapshot.bcc = selectedIdentity->bcc;
        }

        const auto plainBody = messageSnapshot->plainTextBody.has_value()
                                   ? messageSnapshot->plainTextBody->value
                                   : strippedPlainText(messageSnapshot->htmlBody.has_value()
                                                           ? messageSnapshot->htmlBody->value
                                                           : std::string{});
        const auto htmlBody = messageSnapshot->htmlBody.has_value()
                                  ? messageSnapshot->htmlBody->value
                                  : htmlFromText(plainBody);

        switch (request.mode)
        {
        case ComposeMode::Reply:
            snapshot.to = !messageSnapshot->email.replyTo.empty() ? messageSnapshot->email.replyTo
                                                                  : messageSnapshot->email.from;
            snapshot.subject =
                trimSubjectPrefix(messageSnapshot->email.subject.value_or(std::string{}), "Re:");
            snapshot.plainTextBody = buildReplyPlainText(messageSnapshot->email, plainBody);
            snapshot.htmlBody = buildReplyHtml(messageSnapshot->email, htmlBody);
            snapshot.threading.inReplyTo = messageSnapshot->email.messageId;
            snapshot.threading.references = messageSnapshot->email.references;
            snapshot.threading.references.insert(snapshot.threading.references.end(),
                                                 messageSnapshot->email.messageId.begin(),
                                                 messageSnapshot->email.messageId.end());
            break;
        case ComposeMode::ReplyAll:
        {
            std::unordered_set<std::string> excludedEmails;
            if (selectedIdentity.has_value())
            {
                excludedEmails.insert(selectedIdentity->email);
            }
            const auto primaryRecipients = !messageSnapshot->email.replyTo.empty()
                                               ? messageSnapshot->email.replyTo
                                               : messageSnapshot->email.from;
            snapshot.to = primaryRecipients;
            snapshot.cc = deduplicateAddresses(messageSnapshot->email.to, messageSnapshot->email.cc,
                                               excludedEmails);
            snapshot.subject =
                trimSubjectPrefix(messageSnapshot->email.subject.value_or(std::string{}), "Re:");
            snapshot.plainTextBody = buildReplyPlainText(messageSnapshot->email, plainBody);
            snapshot.htmlBody = buildReplyHtml(messageSnapshot->email, htmlBody);
            snapshot.threading.inReplyTo = messageSnapshot->email.messageId;
            snapshot.threading.references = messageSnapshot->email.references;
            snapshot.threading.references.insert(snapshot.threading.references.end(),
                                                 messageSnapshot->email.messageId.begin(),
                                                 messageSnapshot->email.messageId.end());
            break;
        }
        case ComposeMode::Forward:
            snapshot.subject =
                trimSubjectPrefix(messageSnapshot->email.subject.value_or(std::string{}), "Fwd:");
            snapshot.plainTextBody = buildForwardPlainText(messageSnapshot->email, plainBody);
            snapshot.htmlBody = buildForwardHtml(messageSnapshot->email, htmlBody);
            snapshot.attachments = draftAttachmentsFromMessage(messageSnapshot->attachments);
            break;
        case ComposeMode::EditDraft:
            snapshot.draftEmailId = sourceEmailId;
            snapshot.mode = ComposeMode::EditDraft;
            snapshot.editorMode = messageSnapshot->htmlBody.has_value() ? BodyEditorMode::RichText
                                                                        : BodyEditorMode::PlainText;
            snapshot.to = messageSnapshot->email.to;
            snapshot.cc = messageSnapshot->email.cc;
            snapshot.bcc = messageSnapshot->email.bcc;
            snapshot.subject = messageSnapshot->email.subject;
            snapshot.plainTextBody = plainBody;
            snapshot.htmlBody = messageSnapshot->htmlBody.has_value() ? htmlBody : std::string{};
            snapshot.threading.messageId = messageSnapshot->email.messageId;
            snapshot.threading.inReplyTo = messageSnapshot->email.inReplyTo;
            snapshot.threading.references = messageSnapshot->email.references;
            {
                auto attachments = co_await materializeInlineDraftImages(
                    m_connection, m_jmapCore, settings, request.accountId, *sourceEmailId,
                    messageSnapshot->attachments);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&attachments))
                {
                    co_return *error;
                }
                snapshot.attachments =
                    std::get<std::vector<DraftAttachment>>(std::move(attachments));
            }
            break;
        case ComposeMode::NewMessage:
            break;
        }

        if (const auto error = composeRepository.upsert(snapshot))
        {
            co_return javelin::jmap::operationError(*error);
        }

        co_return snapshot;
    }

    QCoro::Task<std::variant<DraftSaveSummary, javelin::jmap::OperationError>>
    ComposeService::saveDraft(javelin::jmap::LiveConnectionSettings settings,
                              DraftSnapshot snapshot, std::optional<std::string> operationGroupId)
    {
        if (const auto validationError = validateSettings(settings))
        {
            co_return *validationError;
        }
        if (!isBoundedComposeSnapshot(snapshot))
        {
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                .message = QStringLiteral("The compose revision exceeds the supported bounds."),
            };
        }

        std::vector<std::optional<QByteArray>> stagedPayloads(snapshot.attachments.size());
        const javelin::jmap::cache::MailVault vault =
            javelin::jmap::cache::MailVault::forDatabase(m_connection);
        for (std::size_t index = 0; index < snapshot.attachments.size(); ++index)
        {
            auto& attachment = snapshot.attachments[index];
            if (attachment.blobId.has_value() || attachment.localFilePath.empty())
                continue;

            QFile file{QString::fromStdString(attachment.localFilePath)};
            if (!file.open(QIODevice::ReadOnly))
            {
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                    .message = QStringLiteral("Failed to stage attachment %1.")
                                   .arg(QString::fromStdString(attachment.localFilePath)),
                };
            }
            const auto payload = file.readAll();
            if (file.error() != QFileDevice::NoError)
            {
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                    .message = QStringLiteral("Failed to read attachment %1.")
                                   .arg(QString::fromStdString(attachment.localFilePath)),
                };
            }
            if (attachment.size != 0 &&
                attachment.size != static_cast<std::uint64_t>(payload.size()))
            {
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::Conflict,
                    .message = QStringLiteral("Attachment %1 changed after it was selected.")
                                   .arg(QString::fromStdString(attachment.localFilePath)),
                };
            }
            const auto contentHash =
                QString::fromLatin1(
                    QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex())
                    .toStdString();
            if (attachment.contentHash.has_value() && *attachment.contentHash != contentHash)
            {
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::Conflict,
                    .message = QStringLiteral("Attachment %1 changed after staging.")
                                   .arg(QString::fromStdString(attachment.localFilePath)),
                };
            }
            const auto staged = vault.stage(payload);
            if (const auto* error = std::get_if<javelin::jmap::cache::MailVaultError>(&staged))
            {
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::LocalStorageFailure,
                    .message = error->message,
                };
            }
            attachment.contentHash = contentHash;
            attachment.size = static_cast<std::uint64_t>(payload.size());
            stagedPayloads[index] = payload;
        }
        if (!isBoundedComposeSnapshot(snapshot))
        {
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                .message =
                    QStringLiteral("The staged compose revision exceeds the supported bounds."),
            };
        }

        const auto admission = m_revisionGate.admit(snapshot.composeSessionId, snapshot.revision,
                                                    acceptedAttachmentManifest(snapshot));
        if (admission != ComposeRevisionAdmission::Accepted)
        {
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::Conflict,
                .message = admission == ComposeRevisionAdmission::Stale
                               ? QStringLiteral("An older compose revision was superseded.")
                               : QStringLiteral(
                                     "The attachment manifest changed for an accepted revision."),
            };
        }

        DraftMutationJournal draftJournal{m_connection};
        const auto activeDraft =
            draftJournal.hasActiveForCompose(snapshot.accountId, snapshot.composeSessionId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&activeDraft))
            co_return javelin::jmap::operationError(*error);
        if (std::get<bool>(activeDraft))
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::Conflict,
                .message =
                    QStringLiteral("The previous draft save is still being reconciled with the "
                                   "server."),
            };

        const auto sessionResult = loadCachedSession(m_connection, snapshot.accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&sessionResult))
        {
            co_return *error;
        }
        const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);

        const auto draftsMailbox = findMailboxByRole(m_connection, snapshot.accountId, "drafts");
        if (!draftsMailbox.has_value())
        {
            co_return javelin::jmap::OperationError{
                .message = QStringLiteral("No Drafts mailbox is available for this account."),
            };
        }

        const auto identitiesResult = co_await ensureIdentities(m_connection, m_methodTransport,
                                                                settings, snapshot.accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&identitiesResult))
        {
            co_return *error;
        }
        const auto& identities =
            std::get<std::vector<javelin::jmap::domain::Identity>>(identitiesResult);
        const auto identityIt = std::find_if(
            identities.cbegin(), identities.cend(), [&snapshot](const auto& identity)
            { return identity.id == snapshot.identityId && !isWildcardSenderIdentity(identity); });
        if (identityIt == identities.cend())
        {
            co_return javelin::jmap::OperationError{
                .message = QStringLiteral("The selected sender identity is unavailable."),
            };
        }

        if (snapshot.editorMode == BodyEditorMode::PlainText)
        {
            snapshot.htmlBody.clear();
        }
        else if (snapshot.htmlBody.empty() && !snapshot.plainTextBody.empty())
        {
            snapshot.htmlBody = htmlFromText(snapshot.plainTextBody);
        }
        if (snapshot.plainTextBody.empty() && !snapshot.htmlBody.empty())
        {
            snapshot.plainTextBody = strippedPlainText(snapshot.htmlBody);
        }

        for (std::size_t index = 0; index < snapshot.attachments.size(); ++index)
        {
            auto& attachment = snapshot.attachments[index];
            if (attachment.blobId.has_value() || attachment.localFilePath.empty())
            {
                if (attachment.displayName.empty() && !attachment.localFilePath.empty())
                {
                    attachment.displayName =
                        QFileInfo{QString::fromStdString(attachment.localFilePath)}
                            .fileName()
                            .toStdString();
                }
                if (attachment.mediaType.empty() && !attachment.localFilePath.empty())
                {
                    attachment.mediaType =
                        attachmentMediaType(QString::fromStdString(attachment.localFilePath));
                }
                continue;
            }

            if (attachment.displayName.empty())
            {
                attachment.displayName = QFileInfo{QString::fromStdString(attachment.localFilePath)}
                                             .fileName()
                                             .toStdString();
            }
            if (attachment.mediaType.empty())
            {
                attachment.mediaType =
                    attachmentMediaType(QString::fromStdString(attachment.localFilePath));
            }

            const auto uploadResult = co_await uploadAttachment(
                m_resourceTransport, settings, session, snapshot.accountId, attachment,
                stagedPayloads[index].value_or(QByteArray{}));
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&uploadResult))
            {
                co_return *error;
            }
            const auto& upload = std::get<UploadSummary>(uploadResult);
            attachment.blobId = upload.blobId;
            attachment.mediaType = upload.type;
            attachment.size = upload.size;
            attachment.contentHash = upload.contentHash;
        }

        javelin::jmap::api::EmailSetRequest request{
            .accountId = snapshot.accountId,
            .ifInState = std::nullopt,
            .create =
                {
                    {"draft",
                     javelin::jmap::api::EmailSetCreate{
                         .mailboxIds = {{draftsMailbox->id, true}},
                         .keywords = {{"$seen", true}, {"$draft", true}},
                         .from = std::optional<std::vector<javelin::jmap::domain::EmailAddress>>{
                             std::vector<javelin::jmap::domain::EmailAddress>{
                                 {
                                     .name = identityIt->name.empty()
                                                 ? std::nullopt
                                                 : std::optional<std::string>{identityIt->name},
                                     .email = identityIt->email,
                                 },
                             }},
                         .to = snapshot.to.empty()
                                   ? std::nullopt
                                   : std::optional<std::vector<javelin::jmap::domain::EmailAddress>>{
                                         snapshot.to},
                         .cc = snapshot.cc.empty()
                                   ? std::nullopt
                                   : std::optional<std::vector<javelin::jmap::domain::EmailAddress>>{
                                         snapshot.cc},
                         .bcc = snapshot.bcc.empty()
                                    ? std::nullopt
                                    : std::optional<std::vector<javelin::jmap::domain::EmailAddress>>{
                                          snapshot.bcc},
                         .replyTo = identityIt->replyTo.empty()
                                        ? std::nullopt
                                        : std::optional<std::vector<
                                              javelin::jmap::domain::EmailAddress>>{
                                              identityIt->replyTo},
                         .subject = snapshot.subject,
                         .receivedAt = nowIsoUtc(),
                         .sentAt = nowIsoUtc(),
                         .messageId = snapshot.threading.messageId.empty()
                                          ? std::nullopt
                                          : std::optional<std::vector<std::string>>{
                                                snapshot.threading.messageId},
                         .inReplyTo = snapshot.threading.inReplyTo.empty()
                                          ? std::nullopt
                                          : std::optional<std::vector<std::string>>{
                                                snapshot.threading.inReplyTo},
                         .references = snapshot.threading.references.empty()
                                           ? std::nullopt
                                           : std::optional<std::vector<std::string>>{
                                                 snapshot.threading.references},
                         .bodyStructure = buildBodyStructure(snapshot),
                         .bodyValues = buildBodyValues(snapshot),
                     }},
                },
            .update = {},
            .destroy = {},
        };
        const auto methodRequest = javelin::jmap::api::emailSet(request);
        if (!methodRequest.has_value())
            co_return javelin::jmap::OperationError{
                .message = QStringLiteral("Failed to encode the Email/set draft request."),
            };

        javelin::jmap::cache::EmailRepository emailRepository{m_connection};
        std::optional<javelin::jmap::domain::Email> baseEmail;
        if (snapshot.draftEmailId.has_value())
        {
            const auto found = emailRepository.find(snapshot.accountId, *snapshot.draftEmailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&found))
                co_return javelin::jmap::operationError(*error);
            baseEmail = std::get<std::optional<javelin::jmap::domain::Email>>(found);
            if (!baseEmail.has_value())
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::LocalStorageFailure,
                    .message = QStringLiteral("The draft being replaced is not cached."),
                };
        }
        const auto resolvedOperationGroupId = operationGroupId.value_or(
            QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());
        const auto temporaryEmailId = std::string{"local-"} + resolvedOperationGroupId;
        const auto temporaryThreadId = std::string{"local-thread-"} + resolvedOperationGroupId;
        const auto projectedEmail = emailFromDraft(
            snapshot, *identityIt, draftsMailbox->id,
            {
                .id = temporaryEmailId,
                .blobId = {},
                .threadId = baseEmail.has_value() ? baseEmail->threadId : temporaryThreadId,
                .size = 0,
            });
        const DraftMutationGroup draftMutation{
            .operationGroupId = resolvedOperationGroupId,
            .createMutationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
            .destroyMutationId =
                snapshot.draftEmailId.has_value()
                    ? std::optional<std::string>{QUuid::createUuid()
                                                     .toString(QUuid::WithoutBraces)
                                                     .toStdString()}
                    : std::nullopt,
            .accountId = snapshot.accountId,
            .temporaryEmailId = temporaryEmailId,
            .replacedEmailId = snapshot.draftEmailId,
            .baseEmail = baseEmail,
            .projectedEmail = projectedEmail,
            .baseSnapshot = snapshot,
        };
        if (const auto error = draftJournal.queue(draftMutation))
            co_return javelin::jmap::operationError(*error);
        if (const auto error =
                draftJournal.transition(draftMutation, sync::MutationStatus::InFlight))
            co_return javelin::jmap::operationError(*error);

        javelin::jmap::api::MethodCaller methodCaller{m_methodTransport};
        javelin::jmap::api::RequestBuilder builder;
        builder.useCore().useMail();
        const auto handle = builder.call(*methodRequest, "draft-save");
        const auto result = co_await methodCaller.call(
            buildApiRequestContext(settings, snapshot.accountId, session), builder);
        if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&result))
        {
            if (const auto transitionError =
                    draftJournal.transition(draftMutation, sync::MutationStatus::Unknown))
                co_return javelin::jmap::operationError(*transitionError);
            co_return javelin::jmap::operationError(*error);
        }
        if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&result))
        {
            if (const auto transitionError =
                    draftJournal.transition(draftMutation, sync::MutationStatus::Pending))
                co_return javelin::jmap::operationError(*transitionError);
            co_return javelin::jmap::operationError(*error);
        }
        if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&result))
        {
            if (const auto transitionError =
                    draftJournal.transition(draftMutation, sync::MutationStatus::Unknown))
                co_return javelin::jmap::operationError(*transitionError);
            co_return javelin::jmap::operationError(*error);
        }

        const auto& envelope = std::get<javelin::jmap::api::ResponseEnvelope>(result);
        const javelin::jmap::api::ResponseReader reader{envelope};
        const auto responseResult = reader.require(handle);
        if (const auto* error =
                std::get_if<javelin::jmap::api::ResponseReaderError>(&responseResult))
        {
            if (error->code == javelin::jmap::api::ResponseReaderErrorCode::MethodError)
            {
                if (const auto rejectionError = draftJournal.rejectCreation(
                        draftMutation,
                        error->methodError.has_value()
                            ? std::optional<std::string_view>{error->methodError->type}
                            : std::nullopt))
                    co_return javelin::jmap::operationError(*rejectionError);
            }
            else if (const auto transitionError =
                         draftJournal.transition(draftMutation, sync::MutationStatus::Unknown))
                co_return javelin::jmap::operationError(*transitionError);
            co_return javelin::jmap::operationError(*error);
        }
        const auto& response = std::get<javelin::jmap::api::EmailSetResponse>(responseResult);
        if (!response.notCreated.empty() || response.created.empty())
        {
            if (const auto rejectionError = draftJournal.rejectCreation(
                    draftMutation, response.notCreated.empty() ? std::optional<std::string_view>{}
                                                               : std::optional<std::string_view>{
                                                                     response.notCreated.front()}))
                co_return javelin::jmap::operationError(*rejectionError);
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::Conflict,
                .message = QStringLiteral("The server rejected the draft save request."),
            };
        }

        const auto createdIt = response.created.find("draft");
        if (createdIt == response.created.end())
        {
            if (const auto transitionError =
                    draftJournal.transition(draftMutation, sync::MutationStatus::Unknown))
                co_return javelin::jmap::operationError(*transitionError);
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::ProtocolViolation,
                .message = QStringLiteral("The draft save response did not include the new draft."),
            };
        }

        const auto synthesizedEmail =
            emailFromDraft(snapshot, *identityIt, draftsMailbox->id, createdIt->second);
        snapshot.draftEmailId = createdIt->second.id;
        if (const auto error = draftJournal.acceptCreation(draftMutation, synthesizedEmail,
                                                           snapshot, response.newState))
            co_return javelin::jmap::operationError(*error);

        if (draftMutation.replacedEmailId.has_value())
        {
            const auto destroyRequest = javelin::jmap::api::emailSet({
                .accountId = snapshot.accountId,
                .ifInState = std::nullopt,
                .create = {},
                .update = {},
                .destroy = {*draftMutation.replacedEmailId},
            });
            if (!destroyRequest.has_value())
            {
                if (const auto transitionError = draftJournal.transitionDestruction(
                        draftMutation, sync::MutationStatus::Unknown))
                    co_return javelin::jmap::operationError(*transitionError);
            }
            else
            {
                javelin::jmap::api::RequestBuilder destroyBuilder;
                destroyBuilder.useCore().useMail();
                const auto destroyHandle =
                    destroyBuilder.call(*destroyRequest, "draft-replace-destroy");
                const auto destroyResult = co_await methodCaller.call(
                    buildApiRequestContext(settings, snapshot.accountId, session), destroyBuilder);
                if (std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(destroyResult))
                {
                    const javelin::jmap::api::ResponseReader destroyReader{
                        std::get<javelin::jmap::api::ResponseEnvelope>(destroyResult)};
                    const auto parsedDestroy = destroyReader.require(destroyHandle);
                    if (const auto* readerError =
                            std::get_if<javelin::jmap::api::ResponseReaderError>(&parsedDestroy))
                    {
                        if (readerError->code ==
                            javelin::jmap::api::ResponseReaderErrorCode::MethodError)
                        {
                            if (const auto rejectionError = draftJournal.rejectDestruction(
                                    draftMutation, response.newState,
                                    readerError->methodError.has_value()
                                        ? std::optional<std::string_view>{readerError->methodError
                                                                              ->type}
                                        : std::nullopt))
                                co_return javelin::jmap::operationError(*rejectionError);
                        }
                        else if (const auto transitionError = draftJournal.transitionDestruction(
                                     draftMutation, sync::MutationStatus::Unknown))
                            co_return javelin::jmap::operationError(*transitionError);
                    }
                    else
                    {
                        const auto& destroyResponse =
                            std::get<javelin::jmap::api::EmailSetResponse>(parsedDestroy);
                        if (std::ranges::find(destroyResponse.destroyed,
                                              *draftMutation.replacedEmailId) !=
                            destroyResponse.destroyed.end())
                        {
                            if (const auto acceptedError = draftJournal.acceptDestruction(
                                    draftMutation, destroyResponse.newState))
                                co_return javelin::jmap::operationError(*acceptedError);
                        }
                        else if (std::ranges::find(destroyResponse.notDestroyed,
                                                   *draftMutation.replacedEmailId) !=
                                 destroyResponse.notDestroyed.end())
                        {
                            if (const auto rejectionError =
                                    draftJournal.rejectDestruction(draftMutation, response.newState,
                                                                   *draftMutation.replacedEmailId))
                                co_return javelin::jmap::operationError(*rejectionError);
                        }
                        else if (const auto transitionError = draftJournal.transitionDestruction(
                                     draftMutation, sync::MutationStatus::Unknown))
                            co_return javelin::jmap::operationError(*transitionError);
                    }
                }
                else if (const auto transitionError = draftJournal.transitionDestruction(
                             draftMutation, sync::MutationStatus::Unknown))
                    co_return javelin::jmap::operationError(*transitionError);
            }
        }

        const auto acceptedManifest = acceptedAttachmentManifest(snapshot);
        if (!m_revisionGate.accept(snapshot.composeSessionId, snapshot.revision, acceptedManifest))
        {
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::Conflict,
                .message = QStringLiteral("A newer compose revision superseded this save."),
            };
        }
        javelin::jmap::cache::ComposeSessionRepository composeRepository{m_connection};
        if (const auto error = composeRepository.upsert(snapshot))
            co_return javelin::jmap::operationError(*error);

        co_return DraftSaveSummary{
            .composeSessionId = snapshot.composeSessionId,
            .accountId = snapshot.accountId,
            .draftEmailId = *snapshot.draftEmailId,
            .affectedMailboxIds = {draftsMailbox->id},
            .operationGroupId = resolvedOperationGroupId,
            .createMutationId = draftMutation.createMutationId,
            .destroyMutationId = draftMutation.destroyMutationId,
            .acceptedRevision = snapshot.revision,
            .acceptedManifest = acceptedManifest,
            .savedSnapshot = snapshot,
        };
    }

    QCoro::Task<std::variant<DraftSnapshot, javelin::jmap::OperationError>>
    ComposeService::loadAuthoritativeDraft(javelin::jmap::LiveConnectionSettings settings,
                                           std::string accountId, std::string draftEmailId,
                                           std::string composeSessionId)
    {
        co_return co_await open(std::move(settings),
                                {
                                    .accountId = std::move(accountId),
                                    .mode = ComposeMode::EditDraft,
                                    .referenceEmailId = std::nullopt,
                                    .draftEmailId = std::move(draftEmailId),
                                    .initialTo = {},
                                    .useExistingWorkingCopy = false,
                                    .composeSessionId = std::move(composeSessionId),
                                });
    }

    QCoro::Task<std::variant<DraftDeleteSummary, javelin::jmap::OperationError>>
    ComposeService::deleteDraft(javelin::jmap::LiveConnectionSettings settings,
                                std::string accountId, std::string draftEmailId,
                                std::string operationGroupId)
    {
        const auto queued = m_jmapCore.queueDestroyEmail(accountId, draftEmailId, operationGroupId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&queued))
            co_return *error;
        const auto& mutation = std::get<javelin::jmap::QueuedEmailMutation>(queued);
        auto submitted = co_await m_jmapCore.submitPendingEmailMutations(
            std::move(settings), accountId, operationGroupId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&submitted))
            co_return *error;
        const auto& summary = std::get<javelin::jmap::SubmittedEmailMutations>(submitted);
        if (summary.updatedEmailCount != 1)
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::Conflict,
                .message = QStringLiteral("The server rejected deleting the saved draft."),
            };
        co_return DraftDeleteSummary{
            .accountId = std::move(accountId),
            .draftEmailId = std::move(draftEmailId),
            .operationGroupId = std::move(operationGroupId),
            .mutationId = mutation.mutationId,
        };
    }

    QCoro::Task<std::variant<SendSummary, javelin::jmap::OperationError>>
    ComposeService::send(javelin::jmap::LiveConnectionSettings settings, DraftSnapshot snapshot)
    {
        auto prepared = co_await prepareSend(settings, std::move(snapshot));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&prepared))
            co_return *error;
        co_return co_await submitPreparedSend(std::move(settings),
                                              std::get<PreparedSend>(std::move(prepared)));
    }

    QCoro::Task<std::variant<PreparedSend, javelin::jmap::OperationError>>
    ComposeService::prepareSend(javelin::jmap::LiveConnectionSettings settings,
                                DraftSnapshot snapshot)
    {
        const auto draftSaveResult = co_await saveDraft(settings, std::move(snapshot));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&draftSaveResult))
            co_return *error;
        auto draft = std::get<DraftSaveSummary>(std::move(draftSaveResult));
        const auto acceptedRevision = draft.acceptedRevision;
        auto acceptedManifest = draft.acceptedManifest;
        co_return PreparedSend{
            .draft = std::move(draft),
            .acceptedRevision = acceptedRevision,
            .acceptedManifest = std::move(acceptedManifest),
        };
    }

    QCoro::Task<std::variant<SendSummary, javelin::jmap::OperationError>>
    ComposeService::submitPreparedSend(javelin::jmap::LiveConnectionSettings settings,
                                       PreparedSend prepared, std::function<void()> dispatched)
    {
        const auto& draftSummary = prepared.draft;

        const auto sessionResult = loadCachedSession(m_connection, draftSummary.accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&sessionResult))
        {
            co_return *error;
        }
        const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);

        const auto draftsMailbox =
            findMailboxByRole(m_connection, draftSummary.accountId, "drafts");
        const auto sentMailbox = findMailboxByRole(m_connection, draftSummary.accountId, "sent");
        if (!draftsMailbox.has_value() || !sentMailbox.has_value())
        {
            co_return javelin::jmap::OperationError{
                .message =
                    QStringLiteral("Both Drafts and Sent mailboxes are required to send mail."),
            };
        }

        const auto draftResult = loadWorkingCopy(draftSummary.composeSessionId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&draftResult))
        {
            co_return *error;
        }
        const auto draftSnapshot = std::get<std::optional<DraftSnapshot>>(draftResult);
        if (!draftSnapshot.has_value())
        {
            co_return javelin::jmap::OperationError{
                .message = QStringLiteral("The compose session is unavailable."),
            };
        }
        const bool hasExplicitAcceptedSnapshot =
            !prepared.draft.savedSnapshot.composeSessionId.empty();
        const auto expectedRevision =
            hasExplicitAcceptedSnapshot ? prepared.acceptedRevision : draftSnapshot->revision;
        const auto expectedManifest = hasExplicitAcceptedSnapshot
                                          ? prepared.acceptedManifest
                                          : acceptedAttachmentManifest(*draftSnapshot);
        if (!hasExplicitAcceptedSnapshot)
            static_cast<void>(m_revisionGate.restoreAccepted(draftSummary.composeSessionId,
                                                             expectedRevision, expectedManifest));
        if (draftSnapshot->revision != expectedRevision ||
            !sameAcceptedManifest(acceptedAttachmentManifest(*draftSnapshot), expectedManifest) ||
            !m_revisionGate.isAccepted(draftSummary.composeSessionId, expectedRevision,
                                       expectedManifest))
        {
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::Conflict,
                .message = QStringLiteral(
                    "The compose revision or attachment manifest is no longer accepted."),
            };
        }

        javelin::jmap::cache::EmailRepository emailRepository{m_connection};
        const auto cachedEmailResult =
            emailRepository.find(draftSummary.accountId, draftSummary.draftEmailId);
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&cachedEmailResult))
            co_return javelin::jmap::operationError(*error);
        const auto& cachedEmail =
            std::get<std::optional<javelin::jmap::domain::Email>>(cachedEmailResult);
        if (!cachedEmail.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::LocalStorageFailure,
                .message = QStringLiteral("The saved draft is missing from the local cache."),
            };

        const auto operationGroupId =
            QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        const auto emailMutationId =
            QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        const auto submissionMutationId =
            QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        const javelin::jmap::sync::EmailMutationRecord emailMutation{
            .mutationId = emailMutationId,
            .operationGroupId = operationGroupId,
            .accountId = draftSummary.accountId,
            .status = javelin::jmap::sync::MutationStatus::Pending,
            .patch =
                {
                    .emailId = draftSummary.draftEmailId,
                    .addMailboxIds = {sentMailbox->id},
                    .removeMailboxIds = {draftsMailbox->id},
                    .addKeywords = {"$seen"},
                    .removeKeywords = {"$draft"},
                    .destroy = false,
                },
            .baseMailboxIds = cachedEmail->mailboxIds,
            .baseKeywords = cachedEmail->keywords,
            .baseState = std::nullopt,
            .acceptedState = std::nullopt,
            .errorJson = std::nullopt,
        };
        const auto submissionObjectId = std::string{"local-"} + submissionMutationId;
        const std::array companionRecords{javelin::jmap::sync::MutationRecord{
            .mutationId = submissionMutationId,
            .operationGroupId = operationGroupId,
            .domain =
                {
                    .accountId = draftSummary.accountId,
                    .dataType = "EmailSubmission",
                },
            .objectId = submissionObjectId,
            .mutationKind = "email_submission_create",
            .status = javelin::jmap::sync::MutationStatus::Pending,
            .payloadJson =
                QJsonDocument{QJsonObject{{QStringLiteral("emailId"),
                                           QString::fromStdString(draftSummary.draftEmailId)}}}
                    .toJson(QJsonDocument::Compact)
                    .toStdString(),
            .baseState = std::nullopt,
            .acceptedState = std::nullopt,
            .errorJson = std::nullopt,
        }};
        javelin::jmap::sync::EmailMutationJournal emailJournal{m_connection};
        const auto projectedEmail =
            javelin::jmap::sync::projectEmailMutations(*cachedEmail, {emailMutation});
        if (const auto error =
                emailJournal.queueGroup(emailMutation, projectedEmail, companionRecords))
            co_return javelin::jmap::operationError(*error);
        const auto transitionGroup = [this, &emailMutationId, &submissionMutationId](
                                         const javelin::jmap::sync::MutationStatus status)
            -> std::optional<javelin::jmap::OperationError>
        {
            auto transactionResult = javelin::jmap::sync::MutationProjectionTransaction::begin(
                m_connection, QStringLiteral("Transition Email submission operation"));
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
                return javelin::jmap::operationError(*error);
            auto transaction = std::get<javelin::jmap::sync::MutationProjectionTransaction>(
                std::move(transactionResult));
            for (const auto& mutationId : {emailMutationId, submissionMutationId})
            {
                if (const auto error = transaction.transition(mutationId, status))
                    return javelin::jmap::operationError(*error);
            }
            if (const auto error = transaction.commit())
                return javelin::jmap::operationError(*error);
            return std::nullopt;
        };
        const auto rejectGroup = [this, &emailMutation, &submissionMutationId,
                                  &cachedEmail](const std::optional<std::string_view> errorJson)
            -> std::optional<javelin::jmap::OperationError>
        {
            auto transactionResult = javelin::jmap::sync::MutationProjectionTransaction::begin(
                m_connection, QStringLiteral("Reject Email submission operation"));
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
                return javelin::jmap::operationError(*error);
            auto transaction = std::get<javelin::jmap::sync::MutationProjectionTransaction>(
                std::move(transactionResult));
            for (const auto& mutationId : {emailMutation.mutationId, submissionMutationId})
            {
                if (const auto error = transaction.transition(
                        mutationId, javelin::jmap::sync::MutationStatus::Rejected, std::nullopt,
                        errorJson))
                    return javelin::jmap::operationError(*error);
            }
            javelin::jmap::cache::EmailRepository emails{m_connection};
            if (const auto error = emails.upsertMany(transaction.cacheTransaction(),
                                                     emailMutation.accountId, {*cachedEmail}))
                return javelin::jmap::operationError(*error);
            if (const auto error = transaction.commit())
                return javelin::jmap::operationError(*error);
            return std::nullopt;
        };
        if (const auto error = transitionGroup(javelin::jmap::sync::MutationStatus::InFlight))
            co_return *error;

        javelin::jmap::api::MethodCaller methodCaller{m_methodTransport};
        javelin::jmap::api::RequestBuilder builder;
        builder.useCore().useMail().useCapability(
            std::string{javelin::jmap::api::submissionCapabilityUri});
        const auto request = javelin::jmap::api::emailSubmissionSet({
            .accountId = draftSummary.accountId,
            .create =
                {
                    {"send",
                     javelin::jmap::api::EmailSubmissionCreate{
                         .identityId = draftSnapshot->identityId,
                         .emailId = draftSummary.draftEmailId,
                         .envelope = std::nullopt,
                     }},
                },
            .onSuccessUpdateEmail =
                {
                    {"#send",
                     {
                         {std::string{"mailboxIds/"} + draftsMailbox->id, nullptr},
                         {std::string{"mailboxIds/"} + sentMailbox->id, true},
                         {"keywords/$draft", nullptr},
                     }},
                },
        });
        if (!request.has_value())
        {
            co_return javelin::jmap::OperationError{
                .message = QStringLiteral("Failed to encode the EmailSubmission/set request."),
            };
        }

        const auto handle = builder.call(*request, "send-message");
        const auto result = co_await methodCaller.call(
            buildApiRequestContext(settings, draftSummary.accountId, session), builder, {},
            std::move(dispatched));
        if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&result))
        {
            if (const auto transitionError =
                    transitionGroup(javelin::jmap::sync::MutationStatus::Unknown))
                co_return *transitionError;
            co_return javelin::jmap::operationError(*error);
        }
        if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&result))
        {
            if (const auto transitionError =
                    transitionGroup(javelin::jmap::sync::MutationStatus::Pending))
                co_return *transitionError;
            co_return javelin::jmap::operationError(*error);
        }
        if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&result))
        {
            if (const auto transitionError =
                    transitionGroup(javelin::jmap::sync::MutationStatus::Unknown))
                co_return *transitionError;
            co_return javelin::jmap::operationError(*error);
        }

        const auto& envelope = std::get<javelin::jmap::api::ResponseEnvelope>(result);
        const javelin::jmap::api::ResponseReader reader{envelope};
        const auto submissionResult = reader.require(handle);
        if (const auto* error =
                std::get_if<javelin::jmap::api::ResponseReaderError>(&submissionResult))
        {
            if (error->code == javelin::jmap::api::ResponseReaderErrorCode::MethodError)
            {
                const auto errorJson =
                    error->methodError.has_value()
                        ? std::optional<std::string_view>{error->methodError->type}
                        : std::nullopt;
                if (const auto rejectionError = rejectGroup(errorJson))
                    co_return *rejectionError;
            }
            else if (const auto transitionError =
                         transitionGroup(javelin::jmap::sync::MutationStatus::Unknown))
                co_return *transitionError;
            co_return javelin::jmap::operationError(*error);
        }

        const auto& submissionResponse =
            std::get<javelin::jmap::api::EmailSubmissionSetResponse>(submissionResult);
        if (!submissionResponse.notCreated.empty() || submissionResponse.created.empty())
        {
            const auto errorJson =
                submissionResponse.notCreated.empty()
                    ? std::optional<std::string_view>{}
                    : std::optional<std::string_view>{submissionResponse.notCreated.front()};
            if (const auto rejectionError = rejectGroup(errorJson))
                co_return *rejectionError;
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::Conflict,
                .message = QStringLiteral("The server rejected the send request."),
            };
        }

        const auto implicitEmailResult =
            reader.require(javelin::jmap::api::CallHandle<javelin::jmap::api::EmailSetResponse>{
                .callId = handle.callId,
            });
        if (const auto* error =
                std::get_if<javelin::jmap::api::ResponseReaderError>(&implicitEmailResult))
        {
            auto transactionResult = javelin::jmap::sync::MutationProjectionTransaction::begin(
                m_connection, QStringLiteral("Reconcile partial Email submission"));
            if (const auto* cacheError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
                co_return javelin::jmap::operationError(*cacheError);
            auto transaction = std::get<javelin::jmap::sync::MutationProjectionTransaction>(
                std::move(transactionResult));
            if (const auto cacheError = transaction.transition(
                    submissionMutationId, javelin::jmap::sync::MutationStatus::Accepted,
                    submissionResponse.newState))
                co_return javelin::jmap::operationError(*cacheError);
            const bool emailRejected =
                error->code == javelin::jmap::api::ResponseReaderErrorCode::MethodError;
            if (const auto cacheError = transaction.transition(
                    emailMutationId,
                    emailRejected ? javelin::jmap::sync::MutationStatus::Rejected
                                  : javelin::jmap::sync::MutationStatus::Unknown,
                    std::nullopt,
                    error->methodError.has_value()
                        ? std::optional<std::string_view>{error->methodError->type}
                        : std::nullopt))
                co_return javelin::jmap::operationError(*cacheError);
            const std::array submissionDomain{javelin::jmap::sync::ConsistencyDomain{
                .accountId = draftSummary.accountId,
                .dataType = "EmailSubmission",
            }};
            if (const auto cacheError = transaction.advance(submissionDomain))
                co_return javelin::jmap::operationError(*cacheError);
            const auto createdSubmission = submissionResponse.created.begin()->second;
            javelin::jmap::cache::SubmissionRepository submissions{m_connection};
            if (const auto cacheError = submissions.upsert(transaction.cacheTransaction(),
                                                           {
                                                               .accountId = draftSummary.accountId,
                                                               .submissionId = createdSubmission.id,
                                                               .emailId = draftSummary.draftEmailId,
                                                               .threadId = std::nullopt,
                                                               .undoStatus = std::nullopt,
                                                               .deliveryStatusJson = std::nullopt,
                                                           }))
                co_return javelin::jmap::operationError(*cacheError);
            if (emailRejected)
            {
                if (const auto cacheError = emailRepository.upsertMany(
                        transaction.cacheTransaction(), draftSummary.accountId, {*cachedEmail}))
                    co_return javelin::jmap::operationError(*cacheError);
            }
            if (const auto cacheError = transaction.remove(submissionMutationId))
                co_return javelin::jmap::operationError(*cacheError);
            if (const auto cacheError = transaction.commit())
                co_return javelin::jmap::operationError(*cacheError);
            if (const auto discardError = discard(draftSummary.composeSessionId))
                co_return *discardError;
            co_return SendSummary{
                .composeSessionId = draftSummary.composeSessionId,
                .accountId = draftSummary.accountId,
                .draftEmailId = draftSummary.draftEmailId,
                .submissionId = createdSubmission.id,
                .acceptedRevision = expectedRevision,
            };
        }
        const auto& implicitEmailResponse =
            std::get<javelin::jmap::api::EmailSetResponse>(implicitEmailResult);

        const auto createdSubmission = submissionResponse.created.begin()->second;
        const bool emailAccepted =
            std::ranges::find(implicitEmailResponse.updated, draftSummary.draftEmailId) !=
            implicitEmailResponse.updated.end();
        const bool emailRejected =
            std::ranges::find(implicitEmailResponse.notUpdated, draftSummary.draftEmailId) !=
            implicitEmailResponse.notUpdated.end();
        auto transactionResult = javelin::jmap::sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Accept Email submission operation"));
        if (const auto* cacheError =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            co_return javelin::jmap::operationError(*cacheError);
        auto transaction = std::get<javelin::jmap::sync::MutationProjectionTransaction>(
            std::move(transactionResult));
        if (const auto cacheError = transaction.transition(
                submissionMutationId, javelin::jmap::sync::MutationStatus::Accepted,
                submissionResponse.newState))
            co_return javelin::jmap::operationError(*cacheError);
        if (const auto cacheError = transaction.transition(
                emailMutationId,
                emailAccepted   ? javelin::jmap::sync::MutationStatus::Accepted
                : emailRejected ? javelin::jmap::sync::MutationStatus::Rejected
                                : javelin::jmap::sync::MutationStatus::Unknown,
                emailAccepted ? std::optional<std::string_view>{implicitEmailResponse.newState}
                              : std::nullopt))
            co_return javelin::jmap::operationError(*cacheError);
        std::vector<javelin::jmap::sync::ConsistencyDomain> acceptedDomains{
            {
                .accountId = draftSummary.accountId,
                .dataType = "EmailSubmission",
            },
        };
        if (emailAccepted)
            acceptedDomains.push_back({
                .accountId = draftSummary.accountId,
                .dataType = "Email",
            });
        if (const auto cacheError = transaction.advance(acceptedDomains))
            co_return javelin::jmap::operationError(*cacheError);
        javelin::jmap::cache::SubmissionRepository submissionRepository{m_connection};
        if (const auto cacheError = submissionRepository.upsert(
                transaction.cacheTransaction(), {
                                                    .accountId = draftSummary.accountId,
                                                    .submissionId = createdSubmission.id,
                                                    .emailId = draftSummary.draftEmailId,
                                                    .threadId = std::nullopt,
                                                    .undoStatus = std::nullopt,
                                                    .deliveryStatusJson = std::nullopt,
                                                }))
            co_return javelin::jmap::operationError(*cacheError);
        if (emailRejected)
        {
            if (const auto cacheError = emailRepository.upsertMany(
                    transaction.cacheTransaction(), draftSummary.accountId, {*cachedEmail}))
                co_return javelin::jmap::operationError(*cacheError);
        }
        if (const auto cacheError = transaction.remove(submissionMutationId))
            co_return javelin::jmap::operationError(*cacheError);
        if (emailAccepted)
        {
            if (const auto cacheError = transaction.remove(emailMutationId))
                co_return javelin::jmap::operationError(*cacheError);
        }
        if (const auto cacheError = transaction.commit())
            co_return javelin::jmap::operationError(*cacheError);

        if (const auto error = discard(draftSummary.composeSessionId))
        {
            co_return *error;
        }

        co_return SendSummary{
            .composeSessionId = draftSummary.composeSessionId,
            .accountId = draftSummary.accountId,
            .draftEmailId = draftSummary.draftEmailId,
            .submissionId = createdSubmission.id,
            .acceptedRevision = expectedRevision,
        };
    }

    std::variant<std::optional<DraftSnapshot>, javelin::jmap::OperationError>
    ComposeService::loadWorkingCopy(const std::string_view composeSessionId) const
    {
        javelin::jmap::cache::ComposeSessionRepository repository{m_connection};
        const auto result = repository.find(composeSessionId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
        {
            return javelin::jmap::operationError(*error);
        }

        return std::get<std::optional<DraftSnapshot>>(result);
    }

    std::optional<javelin::jmap::OperationError>
    ComposeService::storeWorkingCopy(const DraftSnapshot& snapshot)
    {
        javelin::jmap::cache::ComposeSessionRepository repository{m_connection};
        if (const auto error = repository.upsert(snapshot))
        {
            return javelin::jmap::operationError(*error);
        }

        return std::nullopt;
    }

    std::optional<javelin::jmap::OperationError>
    ComposeService::discard(const std::string_view composeSessionId)
    {
        javelin::jmap::cache::ComposeSessionRepository repository{m_connection};
        if (const auto error = repository.remove(composeSessionId))
        {
            return javelin::jmap::operationError(*error);
        }

        return std::nullopt;
    }

} // namespace javelin::jmap::submission
