#pragma once

#include "jmap/cache/Database.h"

#include <QByteArray>

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace javelin::jmap::cache
{

    struct InlinePartPayload
    {
        std::string emailId;
        std::string partId;
        std::string blobId;
        std::string mediaType;
        QByteArray payload;
    };

    class InlinePartPayloadRepository
    {
      public:
        explicit InlinePartPayloadRepository(DatabaseConnection& connection);

        [[nodiscard]] std::optional<DatabaseError>
        upsert(std::string_view accountId, const InlinePartPayload& payload);
        [[nodiscard]] std::variant<std::optional<InlinePartPayload>, DatabaseError>
        find(std::string_view accountId, std::string_view emailId, std::string_view partId) const;

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
