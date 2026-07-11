#include "jmap/submission/ComposeService.h"

#include "jmap/api/MailMethods.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/ResponseReader.h"
#include "jmap/api/Session.h"
#include "jmap/api/Transport.h"
#include "jmap/auth/AccessTokenResolver.h"
#include "jmap/cache/ComposeSessionRepository.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/IdentityRepository.h"
#include "jmap/cache/MessageViewService.h"
#include "jmap/cache/QueryService.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/SubmissionRepository.h"

#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QString>
#include <QTextDocument>
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

        [[nodiscard]] QString authMessage(const javelin::jmap::api::AuthError& error)
        {
            return QStringLiteral("Authentication error (%1): %2")
                .arg(QString::fromUtf8(javelin::jmap::api::toString(error.code).data()),
                     QString::fromStdString(error.message));
        }

        [[nodiscard]] QString transportMessage(const javelin::jmap::api::TransportError& error)
        {
            return QStringLiteral("Transport error (%1): %2")
                .arg(QString::fromUtf8(javelin::jmap::api::toString(error.code).data()),
                     QString::fromStdString(error.message));
        }

        [[nodiscard]] QString protocolMessage(const javelin::jmap::api::ProtocolError& error)
        {
            return QStringLiteral("Protocol error (%1): %2")
                .arg(QString::fromUtf8(javelin::jmap::api::toString(error.code).data()),
                     QString::fromStdString(error.message));
        }

        [[nodiscard]] std::optional<javelin::jmap::LiveRefreshError>
        validateSettings(const javelin::jmap::LiveConnectionSettings& settings)
        {
            if (settings.sessionUrl.empty() || settings.loginEmail.empty() ||
                settings.apiKey.empty())
            {
                return javelin::jmap::LiveRefreshError{
                    .message =
                        QStringLiteral("Session URL, login email, and API key are required."),
                    .requiresUserIntervention = true,
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

        [[nodiscard]] std::variant<javelin::jmap::api::Session, javelin::jmap::LiveRefreshError>
        loadCachedSession(javelin::jmap::cache::DatabaseConnection& connection,
                          const std::string_view accountId)
        {
            javelin::jmap::cache::SessionRepository sessionRepository{connection};
            const auto result = sessionRepository.load(accountId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            {
                return javelin::jmap::LiveRefreshError{.message = error->message};
            }

            const auto& session = std::get<std::optional<javelin::jmap::api::Session>>(result);
            if (!session.has_value())
            {
                return javelin::jmap::LiveRefreshError{
                    .message =
                        QStringLiteral("No cached JMAP session is available for this account."),
                };
            }

            return *session;
        }

        [[nodiscard]] std::variant<std::string, javelin::jmap::LiveRefreshError>
        resolveAccessToken(const javelin::jmap::auth::AccountCredentials& credentials)
        {
            const javelin::jmap::auth::AccessTokenResolver resolver;
            const auto result = resolver.resolve(credentials);
            if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&result))
            {
                return javelin::jmap::LiveRefreshError{
                    .message = authMessage(*error),
                    .requiresUserIntervention = true,
                };
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

        [[nodiscard]] std::string escapeHtml(const QString& value)
        {
            QString escaped = value.toHtmlEscaped();
            escaped.replace(QStringLiteral("\n"), QStringLiteral("<br/>"));
            return escaped.toStdString();
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

        [[nodiscard]] QString joinStrings(const std::vector<std::string>& values)
        {
            QStringList parts;
            for (const auto& value : values)
            {
                parts.push_back(QString::fromStdString(value));
            }
            return parts.join(QStringLiteral(","));
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
            return QStringLiteral("<p><br/></p><div class=\"moz-cite-prefix\">On %1, %2 "
                                  "wrote:<br/></div><blockquote type=\"cite\"%3>%4</blockquote>")
                .arg(sentAt, from, cite, QString::fromStdString(std::string{htmlBody}))
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
            return QStringLiteral(
                       "<p><br/></p><hr/><p><b>From:</b> %1<br/><b>To:</b> %2<br/><b>Subject:</b> "
                       "%3<br/><b>Date:</b> %4</p>%5")
                .arg(QString::fromStdString(joinAddresses(email.from)),
                     QString::fromStdString(joinAddresses(email.to)),
                     QString::fromStdString(email.subject.value_or(std::string{})),
                     QString::fromStdString(email.sentAt.value_or(email.receivedAt)),
                     QString::fromStdString(std::string{htmlBody}))
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
            QTextDocument document;
            document.setHtml(QString::fromStdString(std::string{html}));
            return document.toPlainText().toStdString();
        }

        [[nodiscard]] std::string htmlFromText(const std::string_view plainText)
        {
            return QStringLiteral("<p>%1</p>")
                .arg(QString::fromStdString(
                    escapeHtml(QString::fromStdString(std::string{plainText}))))
                .toStdString();
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
                });
            }
            return draftAttachments;
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
                                               javelin::jmap::LiveRefreshError>>
        ensureIdentities(javelin::jmap::cache::DatabaseConnection& connection,
                         javelin::jmap::api::AbstractTransport& transport,
                         javelin::jmap::LiveConnectionSettings settings, std::string accountId)
        {
            javelin::jmap::cache::IdentityRepository identityRepository{connection};
            const auto cachedResult = identityRepository.listByAccount(accountId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&cachedResult))
            {
                co_return javelin::jmap::LiveRefreshError{.message = error->message};
            }

            const auto& cached =
                std::get<std::vector<javelin::jmap::domain::Identity>>(cachedResult);
            if (!cached.empty())
            {
                co_return cached;
            }

            const auto sessionResult = loadCachedSession(connection, accountId);
            if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&sessionResult))
            {
                co_return *error;
            }
            const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);

            if (!session.capabilities.submission)
            {
                co_return javelin::jmap::LiveRefreshError{
                    .message =
                        QStringLiteral("This account does not advertise JMAP submission support."),
                };
            }

            javelin::jmap::api::MethodCaller methodCaller{transport};
            javelin::jmap::api::RequestBuilder builder;
            builder.useCore().useCapability(
                std::string{javelin::jmap::api::submissionCapabilityUri});
            const auto request = javelin::jmap::api::identityGet({.accountId = accountId,
                                                                  .ids = std::nullopt,
                                                                  .idsReference = std::nullopt,
                                                                  .properties = std::nullopt});
            if (!request.has_value())
            {
                co_return javelin::jmap::LiveRefreshError{
                    .message = QStringLiteral("Failed to encode the Identity/get request."),
                };
            }
            const auto handle = builder.call(*request, "identities");
            const auto envelopeResult = co_await methodCaller.call(
                buildApiRequestContext(settings, accountId, session), builder);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::TransportError>(&envelopeResult))
            {
                co_return javelin::jmap::LiveRefreshError{.message = transportMessage(*error)};
            }
            if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&envelopeResult))
            {
                co_return javelin::jmap::LiveRefreshError{
                    .message = authMessage(*error),
                    .requiresUserIntervention = true,
                };
            }
            if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&envelopeResult))
            {
                co_return javelin::jmap::LiveRefreshError{.message = protocolMessage(*error)};
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
                co_return javelin::jmap::LiveRefreshError{
                    .message = QStringLiteral("Failed to read Identity/get response: %1")
                                   .arg(QString::fromStdString(error->message)),
                };
            }

            const auto& identities =
                std::get<javelin::jmap::api::IdentityGetResponse>(identityResult).list;
            if (const auto error = identityRepository.replaceAll(accountId, identities))
            {
                co_return javelin::jmap::LiveRefreshError{.message = error->message};
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
        };

        [[nodiscard]] QCoro::Task<std::variant<UploadSummary, javelin::jmap::LiveRefreshError>>
        uploadAttachment(javelin::jmap::api::AbstractTransport& transport,
                         javelin::jmap::LiveConnectionSettings settings,
                         javelin::jmap::api::Session session, std::string accountId,
                         DraftAttachment attachment)
        {
            QFile file{QString::fromStdString(attachment.localFilePath)};
            if (!file.open(QIODevice::ReadOnly))
            {
                co_return javelin::jmap::LiveRefreshError{
                    .message = QStringLiteral("Failed to read attachment file %1.")
                                   .arg(QString::fromStdString(attachment.localFilePath)),
                };
            }

            const auto tokenResult = resolveAccessToken(buildCredentials(settings, accountId));
            if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&tokenResult))
            {
                co_return *error;
            }

            const auto body = file.readAll();
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
            });
            if (const auto* error =
                    std::get_if<javelin::jmap::api::TransportError>(&transportResult))
            {
                co_return javelin::jmap::LiveRefreshError{.message = transportMessage(*error)};
            }

            const auto response = std::get<javelin::jmap::api::HttpResponse>(transportResult);
            const auto document = QJsonDocument::fromJson(response.body);
            if (!document.isObject())
            {
                co_return javelin::jmap::LiveRefreshError{
                    .message = QStringLiteral("Failed to decode the attachment upload response."),
                };
            }

            const auto object = document.object();
            const auto blobId = object.value(QStringLiteral("blobId")).toString();
            if (blobId.isEmpty())
            {
                co_return javelin::jmap::LiveRefreshError{
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
                    .partId = std::string{"html-body"},
                    .blobId = std::nullopt,
                    .type = "text/html",
                    .name = std::nullopt,
                    .disposition = std::nullopt,
                    .cid = std::nullopt,
                    .subParts = std::nullopt,
                });
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
                auto part = javelin::jmap::api::EmailBodyPartCreate{
                    .partId = std::nullopt,
                    .blobId = attachment.blobId,
                    .type = attachment.mediaType,
                    .name = attachment.displayName.empty()
                                ? std::nullopt
                                : std::optional<std::string>{attachment.displayName},
                    .disposition = attachment.inlineDisposition
                                       ? std::optional<std::string>{"inline"}
                                       : std::optional<std::string>{"attachment"},
                    .cid = attachment.contentId,
                    .subParts = std::nullopt,
                };
                if (attachment.inlineDisposition && attachment.contentId.has_value())
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
                                   javelin::jmap::api::AbstractTransport& transport,
                                   javelin::jmap::JmapCore& jmapCore)
        : m_connection(connection), m_transport(transport), m_jmapCore(jmapCore)
    {
    }

    QCoro::Task<
        std::variant<std::vector<javelin::jmap::domain::Identity>, javelin::jmap::LiveRefreshError>>
    ComposeService::loadSenderIdentities(javelin::jmap::LiveConnectionSettings settings,
                                         std::string accountId)
    {
        if (const auto validationError = validateSettings(settings))
        {
            co_return *validationError;
        }

        const auto identitiesResult = co_await ensureIdentities(
            m_connection, m_transport, std::move(settings), std::move(accountId));
        if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&identitiesResult))
        {
            co_return *error;
        }

        co_return senderIdentities(
            std::get<std::vector<javelin::jmap::domain::Identity>>(identitiesResult));
    }

    QCoro::Task<std::variant<DraftSnapshot, javelin::jmap::LiveRefreshError>>
    ComposeService::open(javelin::jmap::LiveConnectionSettings settings, OpenComposeRequest request)
    {
        if (const auto validationError = validateSettings(settings))
        {
            co_return *validationError;
        }

        javelin::jmap::cache::ComposeSessionRepository composeRepository{m_connection};
        if (request.draftEmailId.has_value())
        {
            const auto composeSessionsResult = composeRepository.listByAccount(request.accountId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&composeSessionsResult))
            {
                co_return javelin::jmap::LiveRefreshError{.message = error->message};
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
            co_await ensureIdentities(m_connection, m_transport, settings, request.accountId);
        if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&identitiesResult))
        {
            co_return *error;
        }
        const auto& identities =
            std::get<std::vector<javelin::jmap::domain::Identity>>(identitiesResult);
        const auto availableSenderIdentities = senderIdentities(identities);
        if (availableSenderIdentities.empty())
        {
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("No sender identities are available for this account."),
            };
        }

        DraftSnapshot snapshot{
            .composeSessionId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
            .accountId = request.accountId,
            .draftEmailId = std::nullopt,
            .mode = request.mode,
            .editorMode = BodyEditorMode::RichText,
            .identityId = availableSenderIdentities.front().id,
            .to = {},
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
                co_return javelin::jmap::LiveRefreshError{.message = error->message};
            }
            co_return snapshot;
        }

        const auto sourceEmailId =
            request.draftEmailId.has_value() ? request.draftEmailId : request.referenceEmailId;
        if (!sourceEmailId.has_value())
        {
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("A source message id is required."),
            };
        }

        const auto refreshResult =
            co_await m_jmapCore.refreshMessageContent(settings, request.accountId, *sourceEmailId);
        if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&refreshResult))
        {
            co_return *error;
        }

        javelin::jmap::cache::MessageViewService messageViewService{m_connection};
        const auto messageResult = messageViewService.load(request.accountId, *sourceEmailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&messageResult))
        {
            co_return javelin::jmap::LiveRefreshError{.message = error->message};
        }

        const auto& messageSnapshot =
            std::get<std::optional<javelin::jmap::cache::MessageViewSnapshot>>(messageResult);
        if (!messageSnapshot.has_value())
        {
            co_return javelin::jmap::LiveRefreshError{
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
            snapshot.to = messageSnapshot->email.to;
            snapshot.cc = messageSnapshot->email.cc;
            snapshot.bcc = messageSnapshot->email.bcc;
            snapshot.subject = messageSnapshot->email.subject;
            snapshot.plainTextBody = plainBody;
            snapshot.htmlBody = htmlBody;
            snapshot.threading.messageId = messageSnapshot->email.messageId;
            snapshot.threading.inReplyTo = messageSnapshot->email.inReplyTo;
            snapshot.threading.references = messageSnapshot->email.references;
            snapshot.attachments = draftAttachmentsFromMessage(messageSnapshot->attachments);
            break;
        case ComposeMode::NewMessage:
            break;
        }

        if (const auto error = composeRepository.upsert(snapshot))
        {
            co_return javelin::jmap::LiveRefreshError{.message = error->message};
        }

        co_return snapshot;
    }

    QCoro::Task<std::variant<DraftSaveSummary, javelin::jmap::LiveRefreshError>>
    ComposeService::saveDraft(javelin::jmap::LiveConnectionSettings settings,
                              DraftSnapshot snapshot)
    {
        if (const auto validationError = validateSettings(settings))
        {
            co_return *validationError;
        }

        const auto sessionResult = loadCachedSession(m_connection, snapshot.accountId);
        if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&sessionResult))
        {
            co_return *error;
        }
        const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);

        const auto draftsMailbox = findMailboxByRole(m_connection, snapshot.accountId, "drafts");
        if (!draftsMailbox.has_value())
        {
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("No Drafts mailbox is available for this account."),
            };
        }

        const auto identitiesResult =
            co_await ensureIdentities(m_connection, m_transport, settings, snapshot.accountId);
        if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&identitiesResult))
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
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("The selected sender identity is unavailable."),
            };
        }

        if (snapshot.htmlBody.empty() && !snapshot.plainTextBody.empty())
        {
            snapshot.htmlBody = htmlFromText(snapshot.plainTextBody);
        }
        if (snapshot.plainTextBody.empty() && !snapshot.htmlBody.empty())
        {
            snapshot.plainTextBody = strippedPlainText(snapshot.htmlBody);
        }

        for (auto& attachment : snapshot.attachments)
        {
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

            const auto uploadResult = co_await uploadAttachment(m_transport, settings, session,
                                                                snapshot.accountId, attachment);
            if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&uploadResult))
            {
                co_return *error;
            }
            const auto& upload = std::get<UploadSummary>(uploadResult);
            attachment.blobId = upload.blobId;
            attachment.mediaType = upload.type;
            attachment.size = upload.size;
        }

        javelin::jmap::api::EmailSetRequest request{
            .accountId = snapshot.accountId,
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
            .destroy = snapshot.draftEmailId.has_value()
                           ? std::vector<std::string>{*snapshot.draftEmailId}
                           : std::vector<std::string>{},
        };

        javelin::jmap::api::MethodCaller methodCaller{m_transport};
        javelin::jmap::api::RequestBuilder builder;
        builder.useCore().useMail();
        const auto methodRequest = javelin::jmap::api::emailSet(request);
        if (!methodRequest.has_value())
        {
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Failed to encode the Email/set draft request."),
            };
        }
        const auto handle = builder.call(*methodRequest, "draft-save");
        const auto result = co_await methodCaller.call(
            buildApiRequestContext(settings, snapshot.accountId, session), builder);
        if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&result))
        {
            co_return javelin::jmap::LiveRefreshError{.message = transportMessage(*error)};
        }
        if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&result))
        {
            co_return javelin::jmap::LiveRefreshError{
                .message = authMessage(*error),
                .requiresUserIntervention = true,
            };
        }
        if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&result))
        {
            co_return javelin::jmap::LiveRefreshError{.message = protocolMessage(*error)};
        }

        const auto& envelope = std::get<javelin::jmap::api::ResponseEnvelope>(result);
        const javelin::jmap::api::ResponseReader reader{envelope};
        const auto responseResult = reader.require(handle);
        if (const auto* error =
                std::get_if<javelin::jmap::api::ResponseReaderError>(&responseResult))
        {
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Failed to read Email/set draft response: %1")
                               .arg(QString::fromStdString(error->message)),
            };
        }
        const auto& response = std::get<javelin::jmap::api::EmailSetResponse>(responseResult);
        if (!response.notCreated.empty() || response.created.empty())
        {
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("The server rejected the draft save request."),
            };
        }

        const auto createdIt = response.created.find("draft");
        if (createdIt == response.created.end())
        {
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("The draft save response did not include the new draft."),
            };
        }

        javelin::jmap::cache::EmailRepository emailRepository{m_connection};
        const auto synthesizedEmail =
            emailFromDraft(snapshot, *identityIt, draftsMailbox->id, createdIt->second);
        if (const auto error = emailRepository.upsertMany(snapshot.accountId, {synthesizedEmail}))
        {
            co_return javelin::jmap::LiveRefreshError{.message = error->message};
        }
        if (snapshot.draftEmailId.has_value() && *snapshot.draftEmailId != createdIt->second.id)
        {
            const std::vector<std::string> staleDraftIds{*snapshot.draftEmailId};
            if (const auto error = emailRepository.removeMany(snapshot.accountId, staleDraftIds))
            {
                co_return javelin::jmap::LiveRefreshError{.message = error->message};
            }
        }

        snapshot.draftEmailId = createdIt->second.id;
        javelin::jmap::cache::ComposeSessionRepository composeRepository{m_connection};
        if (const auto error = composeRepository.upsert(snapshot))
        {
            co_return javelin::jmap::LiveRefreshError{.message = error->message};
        }

        co_return DraftSaveSummary{
            .composeSessionId = snapshot.composeSessionId,
            .accountId = snapshot.accountId,
            .draftEmailId = *snapshot.draftEmailId,
        };
    }

    QCoro::Task<std::variant<SendSummary, javelin::jmap::LiveRefreshError>>
    ComposeService::send(javelin::jmap::LiveConnectionSettings settings, DraftSnapshot snapshot)
    {
        const auto draftSaveResult = co_await saveDraft(settings, std::move(snapshot));
        if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&draftSaveResult))
        {
            co_return *error;
        }
        const auto& draftSummary = std::get<DraftSaveSummary>(draftSaveResult);

        const auto sessionResult = loadCachedSession(m_connection, draftSummary.accountId);
        if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&sessionResult))
        {
            co_return *error;
        }
        const auto& session = std::get<javelin::jmap::api::Session>(sessionResult);

        const auto draftsMailbox =
            findMailboxByRole(m_connection, draftSummary.accountId, "drafts");
        const auto sentMailbox = findMailboxByRole(m_connection, draftSummary.accountId, "sent");
        if (!draftsMailbox.has_value() || !sentMailbox.has_value())
        {
            co_return javelin::jmap::LiveRefreshError{
                .message =
                    QStringLiteral("Both Drafts and Sent mailboxes are required to send mail."),
            };
        }

        const auto draftResult = loadWorkingCopy(draftSummary.composeSessionId);
        if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&draftResult))
        {
            co_return *error;
        }
        const auto draftSnapshot = std::get<std::optional<DraftSnapshot>>(draftResult);
        if (!draftSnapshot.has_value())
        {
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("The compose session is unavailable."),
            };
        }

        javelin::jmap::api::MethodCaller methodCaller{m_transport};
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
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Failed to encode the EmailSubmission/set request."),
            };
        }

        const auto handle = builder.call(*request, "send-message");
        qInfo().noquote() << "Compose send submitting draft"
                          << QString::fromStdString(draftSummary.accountId)
                          << QString::fromStdString(draftSummary.draftEmailId) << "draftsMailbox"
                          << QString::fromStdString(draftsMailbox->id) << "sentMailbox"
                          << QString::fromStdString(sentMailbox->id);
        const auto result = co_await methodCaller.call(
            buildApiRequestContext(settings, draftSummary.accountId, session), builder);
        if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&result))
        {
            co_return javelin::jmap::LiveRefreshError{.message = transportMessage(*error)};
        }
        if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&result))
        {
            co_return javelin::jmap::LiveRefreshError{
                .message = authMessage(*error),
                .requiresUserIntervention = true,
            };
        }
        if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&result))
        {
            co_return javelin::jmap::LiveRefreshError{.message = protocolMessage(*error)};
        }

        const auto& envelope = std::get<javelin::jmap::api::ResponseEnvelope>(result);
        const javelin::jmap::api::ResponseReader reader{envelope};
        for (const auto& response : reader.rawAll(handle.callId))
        {
            qInfo().noquote() << "Compose send raw response"
                              << QString::fromStdString(response.name) << "callId"
                              << QString::fromStdString(response.callId) << "arguments"
                              << QString::fromStdString(response.arguments);
        }
        const auto submissionResult = reader.require(handle);
        if (const auto* error =
                std::get_if<javelin::jmap::api::ResponseReaderError>(&submissionResult))
        {
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Failed to read EmailSubmission/set response: %1")
                               .arg(QString::fromStdString(error->message)),
            };
        }

        const auto& submissionResponse =
            std::get<javelin::jmap::api::EmailSubmissionSetResponse>(submissionResult);
        qInfo().noquote() << "Compose send submission response"
                          << QString::fromStdString(draftSummary.accountId) << "created"
                          << submissionResponse.created.size() << "notCreated"
                          << joinStrings(submissionResponse.notCreated);
        if (!submissionResponse.notCreated.empty() || submissionResponse.created.empty())
        {
            co_return javelin::jmap::LiveRefreshError{
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
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Failed to read the implicit Email/set send response: %1")
                               .arg(QString::fromStdString(error->message)),
            };
        }
        const auto& implicitEmailResponse =
            std::get<javelin::jmap::api::EmailSetResponse>(implicitEmailResult);
        qInfo().noquote() << "Compose send implicit Email/set response"
                          << QString::fromStdString(draftSummary.accountId) << "updated"
                          << joinStrings(implicitEmailResponse.updated) << "destroyed"
                          << joinStrings(implicitEmailResponse.destroyed) << "notUpdated"
                          << joinStrings(implicitEmailResponse.notUpdated) << "notDestroyed"
                          << joinStrings(implicitEmailResponse.notDestroyed);

        const auto cleanupRequest = javelin::jmap::api::emailSet({
            .accountId = draftSummary.accountId,
            .create = {},
            .update =
                {
                    {draftSummary.draftEmailId,
                     javelin::jmap::api::EmailSetUpdate{
                         .mailboxIds = {{draftsMailbox->id, nullptr}, {sentMailbox->id, true}},
                         .keywords = {{"$draft", nullptr}, {"$seen", true}},
                     }},
                },
            .destroy = {},
        });
        if (!cleanupRequest.has_value())
        {
            qWarning().noquote() << "Compose send failed to encode explicit draft cleanup"
                                 << QString::fromStdString(draftSummary.accountId)
                                 << QString::fromStdString(draftSummary.draftEmailId);
        }
        else
        {
            javelin::jmap::api::RequestBuilder cleanupBuilder;
            cleanupBuilder.useCore().useMail();
            const auto cleanupHandle = cleanupBuilder.call(*cleanupRequest, "send-cleanup");
            const auto cleanupResult = co_await methodCaller.call(
                buildApiRequestContext(settings, draftSummary.accountId, session), cleanupBuilder);
            if (const auto* transportError =
                    std::get_if<javelin::jmap::api::TransportError>(&cleanupResult))
            {
                qWarning().noquote() << "Compose send explicit draft cleanup transport failure"
                                     << transportMessage(*transportError);
            }
            else if (const auto* authError =
                         std::get_if<javelin::jmap::api::AuthError>(&cleanupResult))
            {
                qWarning().noquote() << "Compose send explicit draft cleanup auth failure"
                                     << authMessage(*authError);
            }
            else if (const auto* protocolError =
                         std::get_if<javelin::jmap::api::ProtocolError>(&cleanupResult))
            {
                qWarning().noquote() << "Compose send explicit draft cleanup protocol failure"
                                     << protocolMessage(*protocolError);
            }
            else
            {
                const auto& cleanupEnvelope =
                    std::get<javelin::jmap::api::ResponseEnvelope>(cleanupResult);
                const javelin::jmap::api::ResponseReader cleanupReader{cleanupEnvelope};
                for (const auto& response : cleanupReader.rawAll(cleanupHandle.callId))
                {
                    qInfo().noquote() << "Compose send cleanup raw response"
                                      << QString::fromStdString(response.name) << "callId"
                                      << QString::fromStdString(response.callId) << "arguments"
                                      << QString::fromStdString(response.arguments);
                }
                const auto cleanupResponseResult = cleanupReader.require(cleanupHandle);
                if (const auto* readerError = std::get_if<javelin::jmap::api::ResponseReaderError>(
                        &cleanupResponseResult))
                {
                    qWarning().noquote() << "Compose send explicit draft cleanup response failure"
                                         << QString::fromStdString(readerError->message);
                }
                else
                {
                    const auto& cleanupResponse =
                        std::get<javelin::jmap::api::EmailSetResponse>(cleanupResponseResult);
                    qInfo().noquote() << "Compose send explicit draft cleanup response"
                                      << QString::fromStdString(draftSummary.accountId) << "updated"
                                      << joinStrings(cleanupResponse.updated) << "notUpdated"
                                      << joinStrings(cleanupResponse.notUpdated);
                    if (!cleanupResponse.notUpdated.empty())
                    {
                        qWarning().noquote() << "Compose send explicit draft cleanup rejected for"
                                             << joinStrings(cleanupResponse.notUpdated);
                    }
                }
            }
        }

        javelin::jmap::cache::SubmissionRepository submissionRepository{m_connection};
        const auto createdSubmission = submissionResponse.created.begin()->second;
        if (const auto error = submissionRepository.upsert({
                .accountId = draftSummary.accountId,
                .submissionId = createdSubmission.id,
                .emailId = draftSummary.draftEmailId,
                .threadId = std::nullopt,
                .undoStatus = std::nullopt,
                .deliveryStatusJson = std::nullopt,
            }))
        {
            co_return javelin::jmap::LiveRefreshError{.message = error->message};
        }

        javelin::jmap::cache::EmailRepository emailRepository{m_connection};
        const auto emailResult =
            emailRepository.find(draftSummary.accountId, draftSummary.draftEmailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&emailResult))
        {
            co_return javelin::jmap::LiveRefreshError{.message = error->message};
        }
        if (const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(emailResult);
            email.has_value())
        {
            qInfo().noquote() << "Compose send local cache before update"
                              << QString::fromStdString(draftSummary.accountId)
                              << QString::fromStdString(draftSummary.draftEmailId) << "mailboxes"
                              << joinStrings(email->mailboxIds) << "keywords"
                              << joinStrings(email->keywords);
            auto updatedEmail = *email;
            updatedEmail.mailboxIds.erase(std::remove(updatedEmail.mailboxIds.begin(),
                                                      updatedEmail.mailboxIds.end(),
                                                      draftsMailbox->id),
                                          updatedEmail.mailboxIds.end());
            if (std::find(updatedEmail.mailboxIds.cbegin(), updatedEmail.mailboxIds.cend(),
                          sentMailbox->id) == updatedEmail.mailboxIds.cend())
            {
                updatedEmail.mailboxIds.push_back(sentMailbox->id);
            }
            updatedEmail.keywords.erase(
                std::remove(updatedEmail.keywords.begin(), updatedEmail.keywords.end(), "$draft"),
                updatedEmail.keywords.end());
            if (const auto error =
                    emailRepository.upsertMany(draftSummary.accountId, {updatedEmail}))
            {
                co_return javelin::jmap::LiveRefreshError{.message = error->message};
            }
            qInfo().noquote() << "Compose send local cache after update"
                              << QString::fromStdString(draftSummary.accountId)
                              << QString::fromStdString(draftSummary.draftEmailId) << "mailboxes"
                              << joinStrings(updatedEmail.mailboxIds) << "keywords"
                              << joinStrings(updatedEmail.keywords);
        }
        else
        {
            qWarning().noquote() << "Compose send local draft missing after submission"
                                 << QString::fromStdString(draftSummary.accountId)
                                 << QString::fromStdString(draftSummary.draftEmailId);
        }

        if (const auto error = discard(draftSummary.composeSessionId))
        {
            co_return *error;
        }

        co_return SendSummary{
            .composeSessionId = draftSummary.composeSessionId,
            .accountId = draftSummary.accountId,
            .draftEmailId = draftSummary.draftEmailId,
            .submissionId = createdSubmission.id,
        };
    }

    std::variant<std::optional<DraftSnapshot>, javelin::jmap::LiveRefreshError>
    ComposeService::loadWorkingCopy(const std::string_view composeSessionId) const
    {
        javelin::jmap::cache::ComposeSessionRepository repository{m_connection};
        const auto result = repository.find(composeSessionId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
        {
            return javelin::jmap::LiveRefreshError{.message = error->message};
        }

        return std::get<std::optional<DraftSnapshot>>(result);
    }

    std::optional<javelin::jmap::LiveRefreshError>
    ComposeService::storeWorkingCopy(const DraftSnapshot& snapshot)
    {
        javelin::jmap::cache::ComposeSessionRepository repository{m_connection};
        if (const auto error = repository.upsert(snapshot))
        {
            return javelin::jmap::LiveRefreshError{.message = error->message};
        }

        return std::nullopt;
    }

    std::optional<javelin::jmap::LiveRefreshError>
    ComposeService::discard(const std::string_view composeSessionId)
    {
        javelin::jmap::cache::ComposeSessionRepository repository{m_connection};
        if (const auto error = repository.remove(composeSessionId))
        {
            return javelin::jmap::LiveRefreshError{.message = error->message};
        }

        return std::nullopt;
    }

} // namespace javelin::jmap::submission
