#pragma once

#include "jmap/cache/MessageContentTypes.h"
#include "jmap/cache/MessageViewService.h"

#include <QByteArray>
#include <QString>

#include <optional>
#include <string_view>
#include <vector>

namespace javelin::jmap::cache
{

    struct ParsedMessageSource
    {
        std::optional<MessageBody> plainTextBody;
        std::optional<MessageBody> htmlBody;
        std::vector<EmailPart> renderParts;
        std::vector<MessageAttachment> attachments;
    };

    struct ParsedMessagePart
    {
        EmailPart part;
        QByteArray payload;
    };

    struct SearchableMessageBody
    {
        QString text;
        bool isHtml = false;
    };

    [[nodiscard]] ParsedMessageSource parseMessageSource(std::string_view emailId,
                                                         const QByteArray& payload);
    [[nodiscard]] std::optional<SearchableMessageBody>
    parseSearchableMessageBody(QByteArray payload);
    [[nodiscard]] std::optional<ParsedMessagePart> findMessageSourcePart(std::string_view emailId,
                                                                         const QByteArray& payload,
                                                                         std::string_view partId);

} // namespace javelin::jmap::cache
