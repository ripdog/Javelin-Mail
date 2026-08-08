#include "jmap/cache/MimeMessageParser.h"

#include <KMime/Message>
#include <KMime/Util>

#include <QUrl>

namespace javelin::jmap::cache
{
    namespace
    {
        [[nodiscard]] std::string contentPartId(const KMime::Content& content)
        {
            const auto index = content.index().toString();
            return index.isEmpty() ? std::string{"1"} : index.toStdString();
        }

        [[nodiscard]] std::string mediaType(const KMime::Content& content)
        {
            const auto* type = content.contentType();
            if (type == nullptr || type->mimeType().isEmpty())
            {
                return std::string{"text/plain"};
            }

            return type->mimeType().toStdString();
        }

        [[nodiscard]] std::optional<std::string> contentName(const KMime::Content& content)
        {
            if (const auto* disposition = content.contentDisposition();
                disposition != nullptr && !disposition->filename().isEmpty())
            {
                return disposition->filename().toStdString();
            }

            if (const auto* type = content.contentType();
                type != nullptr && !type->name().isEmpty())
            {
                return type->name().toStdString();
            }

            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::string> contentDisposition(const KMime::Content& content)
        {
            const auto* disposition = content.contentDisposition();
            if (disposition == nullptr)
            {
                return std::nullopt;
            }

            switch (disposition->disposition())
            {
            case KMime::Headers::CDinline:
                return std::string{"inline"};
            case KMime::Headers::CDattachment:
                return std::string{"attachment"};
            default:
                return std::nullopt;
            }
        }

        [[nodiscard]] std::optional<std::string> contentId(const KMime::Content& content)
        {
            const auto* id = content.contentID();
            if (id == nullptr || id->identifier().isEmpty())
            {
                return std::nullopt;
            }

            return id->identifier().toStdString();
        }

        [[nodiscard]] bool isInlineRenderable(const std::string_view type)
        {
            return type.starts_with("image/") || type.starts_with("audio/") ||
                   type.starts_with("video/");
        }

        [[nodiscard]] EmailPart partFromContent(const KMime::Content& content,
                                                const std::string_view emailId)
        {
            const auto type = mediaType(content);
            const auto name = contentName(content);
            const auto disposition = contentDisposition(content);
            const auto attachment = KMime::isAttachment(&content) || name.has_value() ||
                                    disposition == std::optional<std::string>{"attachment"};
            const auto partId = contentPartId(content);
            return EmailPart{
                .emailId = std::string{emailId},
                .partId = partId,
                .parentPartId = std::nullopt,
                .blobId = partId,
                .kind = attachment ? "attachment" : "inline",
                .mediaType = type,
                .name = name,
                .charset = std::nullopt,
                .disposition = disposition,
                .cid = contentId(content),
                .size = static_cast<std::uint64_t>(content.decodedBody().size()),
                .isInlineRenderable = isInlineRenderable(type),
                .isBodySection = false,
            };
        }

        void collectParts(const KMime::Content& content, const std::string_view emailId,
                          ParsedMessageSource& parsed)
        {
            for (const auto* child : content.contents())
            {
                if (child != nullptr)
                {
                    collectParts(*child, emailId, parsed);
                }
            }

            auto part = partFromContent(content, emailId);
            if (part.mediaType == "text/plain" && !parsed.plainTextBody.has_value())
            {
                parsed.plainTextBody = MessageBody{
                    .kind = MessageBodyKind::PlainText,
                    .partId = part.partId,
                    .isTruncated = false,
                    .value = content.decodedText().toStdString(),
                };
            }
            else if (part.mediaType == "text/html" && !parsed.htmlBody.has_value())
            {
                parsed.htmlBody = MessageBody{
                    .kind = MessageBodyKind::Html,
                    .partId = part.partId,
                    .isTruncated = false,
                    .value = content.decodedText().toStdString(),
                };
            }

            const bool attachment = KMime::isAttachment(&content) || part.name.has_value() ||
                                    part.disposition == std::optional<std::string>{"attachment"};
            if (!part.cid.has_value() && !attachment)
            {
                return;
            }

            parsed.renderParts.push_back(part);
            if (attachment)
            {
                parsed.attachments.push_back(MessageAttachment{
                    .partId = part.partId,
                    .blobId = part.blobId,
                    .mediaType = part.mediaType,
                    .name = part.name,
                    .disposition = part.disposition,
                    .cid = part.cid,
                    .size = part.size,
                    .isInlineRenderable = part.isInlineRenderable,
                });
            }
        }

        [[nodiscard]] std::optional<MessageBody> bodyFromPart(const KMime::Message& message,
                                                              const QByteArray& type,
                                                              const MessageBodyKind kind)
        {
            const auto* part = message.mainBodyPart(type);
            if (part == nullptr)
            {
                return std::nullopt;
            }

            return MessageBody{
                .kind = kind,
                .partId = contentPartId(*part),
                .isTruncated = false,
                .value = part->decodedText().toStdString(),
            };
        }

        void parseMessage(KMime::Message& message, const QByteArray& payload)
        {
            message.setContent(KMime::CRLFtoLF(payload));
            message.parse();
        }

        [[nodiscard]] std::optional<std::string>
        mailingListUnsubscribeUrl(const KMime::Message& message)
        {
            const auto* header = message.headerByType(QByteArrayLiteral("List-Unsubscribe"));
            if (header == nullptr)
            {
                return std::nullopt;
            }

            const auto value = header->asUnicodeString();
            std::optional<std::string> mailtoUrl;
            qsizetype offset = 0;
            while (offset < value.size())
            {
                const auto opening = value.indexOf(QLatin1Char('<'), offset);
                if (opening < 0)
                {
                    break;
                }
                const auto closing = value.indexOf(QLatin1Char('>'), opening + 1);
                if (closing < 0)
                {
                    break;
                }

                const auto candidate = value.mid(opening + 1, closing - opening - 1).trimmed();
                const QUrl url{candidate};
                if (url.isValid() && !url.scheme().isEmpty())
                {
                    const auto scheme = url.scheme().toLower();
                    const auto encoded = url.toString(QUrl::FullyEncoded).toStdString();
                    if (scheme == QStringLiteral("https") || scheme == QStringLiteral("http"))
                    {
                        return encoded;
                    }
                    if (scheme == QStringLiteral("mailto") && !mailtoUrl.has_value())
                    {
                        mailtoUrl = encoded;
                    }
                }

                offset = closing + 1;
            }
            return mailtoUrl;
        }

        void normalizeCrlfInPlace(QByteArray& payload)
        {
            char* data = payload.data();
            const qsizetype size = payload.size();
            qsizetype writeOffset = 0;
            for (qsizetype readOffset = 0; readOffset < size; ++readOffset)
            {
                if (data[readOffset] == '\r' && readOffset + 1 < size &&
                    data[readOffset + 1] == '\n')
                    continue;
                data[writeOffset++] = data[readOffset];
            }
            payload.truncate(writeOffset);
        }

        [[nodiscard]] const KMime::Content* findPart(const KMime::Content& content,
                                                     const std::string_view partId)
        {
            if (contentPartId(content) == partId)
            {
                return &content;
            }

            for (const auto* child : content.contents())
            {
                if (child == nullptr)
                {
                    continue;
                }
                if (const auto* found = findPart(*child, partId); found != nullptr)
                {
                    return found;
                }
            }

            return nullptr;
        }

    } // namespace

    ParsedMessageSource parseMessageSource(const std::string_view emailId,
                                           const QByteArray& payload)
    {
        KMime::Message message;
        parseMessage(message, payload);
        ParsedMessageSource parsed{
            .plainTextBody =
                bodyFromPart(message, QByteArrayLiteral("text/plain"), MessageBodyKind::PlainText),
            .htmlBody =
                bodyFromPart(message, QByteArrayLiteral("text/html"), MessageBodyKind::Html),
            .unsubscribeUrl = mailingListUnsubscribeUrl(message),
            .renderParts = {},
            .attachments = {},
        };
        collectParts(message, emailId, parsed);
        return parsed;
    }

    std::optional<SearchableMessageBody> parseSearchableMessageBody(QByteArray payload)
    {
        normalizeCrlfInPlace(payload);
        KMime::Message message;
        message.setContent(payload);
        message.parse();

        if (const auto* plain = message.mainBodyPart(QByteArrayLiteral("text/plain"));
            plain != nullptr)
        {
            return SearchableMessageBody{.text = plain->decodedText(), .isHtml = false};
        }
        if (const auto* html = message.mainBodyPart(QByteArrayLiteral("text/html"));
            html != nullptr)
            return SearchableMessageBody{.text = html->decodedText(), .isHtml = true};
        return std::nullopt;
    }

    std::optional<ParsedMessagePart> findMessageSourcePart(const std::string_view emailId,
                                                           const QByteArray& payload,
                                                           const std::string_view partId)
    {
        KMime::Message message;
        parseMessage(message, payload);
        const auto* content = findPart(message, partId);
        if (content == nullptr)
        {
            return std::nullopt;
        }

        return ParsedMessagePart{
            .part = partFromContent(*content, emailId),
            .payload = content->decodedBody(),
        };
    }

} // namespace javelin::jmap::cache
