#include "jmap/cache/MessageViewService.h"

#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MessageContentRepository.h"
#include "jmap/render/HtmlMessageDocumentBuilder.h"

#include <unordered_map>

namespace javelin::jmap::cache
{

    MessageViewService::MessageViewService(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::variant<std::optional<MessageViewSnapshot>, DatabaseError>
    MessageViewService::load(const std::string_view accountId, const std::string_view emailId) const
    {
        EmailRepository emailRepository{m_connection};
        MessageContentRepository contentRepository{m_connection};

        const auto emailResult = emailRepository.find(accountId, emailId);
        if (const auto* error = std::get_if<DatabaseError>(&emailResult))
        {
            return *error;
        }

        const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(emailResult);
        if (!email.has_value())
        {
            return std::optional<MessageViewSnapshot>{std::nullopt};
        }

        const auto partsResult = contentRepository.loadParts(accountId, emailId);
        if (const auto* error = std::get_if<DatabaseError>(&partsResult))
        {
            return *error;
        }

        const auto bodyValuesResult = contentRepository.loadBodyValues(accountId, emailId);
        if (const auto* error = std::get_if<DatabaseError>(&bodyValuesResult))
        {
            return *error;
        }

        MessageViewSnapshot snapshot{
            .email = *email,
            .plainTextBody = std::nullopt,
            .htmlBody = std::nullopt,
            .htmlRenderDocument = std::nullopt,
            .attachments = {},
        };

        const auto& parts = std::get<std::vector<EmailPart>>(partsResult);
        const auto& bodyValues = std::get<std::vector<EmailBodyValue>>(bodyValuesResult);

        std::unordered_map<std::string, const EmailPart*> partsById;
        partsById.reserve(parts.size());
        for (const auto& part : parts)
        {
            partsById.emplace(part.partId, &part);

            const bool isAttachment =
                !part.isBodySection &&
                (part.kind == "attachment" || part.name.has_value() || part.blobId.has_value() ||
                 part.disposition.has_value() || part.cid.has_value());
            if (!isAttachment)
            {
                continue;
            }

            snapshot.attachments.push_back(MessageAttachment{
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

        for (const auto& bodyValue : bodyValues)
        {
            const auto partIt = partsById.find(bodyValue.partId);
            if (partIt == partsById.end())
            {
                continue;
            }

            const auto* part = partIt->second;
            if (!part->isBodySection)
            {
                continue;
            }

            const MessageBody body{
                .kind = part->mediaType == "text/html" ? MessageBodyKind::Html
                                                       : MessageBodyKind::PlainText,
                .partId = bodyValue.partId,
                .isTruncated = bodyValue.isTruncated,
                .value = bodyValue.value,
            };

            if (part->mediaType == "text/plain" && !snapshot.plainTextBody.has_value())
            {
                snapshot.plainTextBody = body;
            }
            else if (part->mediaType == "text/html" && !snapshot.htmlBody.has_value())
            {
                snapshot.htmlBody = body;
            }
        }

        if (snapshot.htmlBody.has_value())
        {
            javelin::jmap::render::HtmlMessageDocumentBuilder builder;
            snapshot.htmlRenderDocument =
                builder.build(accountId, emailId, snapshot.htmlBody->value, parts);
        }

        return std::optional<MessageViewSnapshot>{std::move(snapshot)};
    }

} // namespace javelin::jmap::cache
