#pragma once

#include "jmap/cache/Database.h"
#include "jmap/domain/MailEntities.h"
#include "jmap/language/LanguageDetection.h"
#include "jmap/render/HtmlMessageDocumentBuilder.h"

#include <QFuture>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    enum class MessageBodyKind
    {
        PlainText,
        Html,
    };

    struct MessageBody
    {
        MessageBodyKind kind = MessageBodyKind::PlainText;
        std::string partId;
        bool isTruncated = false;
        std::string value;
    };

    struct MessageAttachment
    {
        std::string partId;
        std::optional<std::string> blobId;
        std::string mediaType;
        std::optional<std::string> name;
        std::optional<std::string> disposition;
        std::optional<std::string> cid;
        std::uint64_t size = 0;
        bool isInlineRenderable = false;
    };

    struct MessageViewSnapshot
    {
        javelin::jmap::domain::Email email;
        std::optional<MessageBody> plainTextBody;
        std::optional<MessageBody> htmlBody;
        std::optional<javelin::jmap::render::HtmlRenderDocument> htmlRenderDocument;
        std::optional<javelin::jmap::language::LanguageDetectionResult> languageDetection;
        bool shouldOfferTranslation = false;
        std::vector<MessageAttachment> attachments;
    };

    using MessageViewResult = std::variant<std::optional<MessageViewSnapshot>, DatabaseError>;

    class MessageViewService
    {
      public:
        explicit MessageViewService(DatabaseConnection& connection);

        [[nodiscard]] MessageViewResult load(std::string_view accountId,
                                             std::string_view emailId) const;
        [[nodiscard]] QFuture<MessageViewResult> loadAsync(std::string accountId,
                                                           std::string emailId) const;

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
