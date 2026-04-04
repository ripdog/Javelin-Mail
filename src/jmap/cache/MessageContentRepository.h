#pragma once

#include "jmap/cache/Database.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    struct EmailPart
    {
        std::string emailId;
        std::string partId;
        std::optional<std::string> parentPartId;
        std::optional<std::string> blobId;
        std::string kind;
        std::string mediaType;
        std::optional<std::string> name;
        std::optional<std::string> charset;
        std::optional<std::string> disposition;
        std::optional<std::string> cid;
        std::uint64_t size = 0;
        bool isInlineRenderable = false;
        bool isBodySection = false;
    };

    struct EmailBodyValue
    {
        std::string emailId;
        std::string partId;
        std::optional<std::string> blobId;
        bool isTruncated = false;
        std::string value;
    };

    class MessageContentRepository
    {
      public:
        explicit MessageContentRepository(DatabaseConnection& connection);

        [[nodiscard]] std::optional<DatabaseError>
        replaceForEmail(std::string_view accountId, std::string_view emailId,
                        const std::vector<EmailPart>& parts,
                        const std::vector<EmailBodyValue>& bodyValues);
        [[nodiscard]] std::variant<std::vector<EmailPart>, DatabaseError>
        loadParts(std::string_view accountId, std::string_view emailId) const;
        [[nodiscard]] std::variant<std::vector<EmailBodyValue>, DatabaseError>
        loadBodyValues(std::string_view accountId, std::string_view emailId) const;

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
