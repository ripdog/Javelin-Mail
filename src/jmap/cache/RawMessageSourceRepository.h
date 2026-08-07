#pragma once

#include "jmap/cache/Database.h"

#include <QByteArray>

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace javelin::jmap::cache
{

    struct MailVaultObject;

    struct RawMessageSource
    {
        std::string emailId;
        std::string blobId;
        QByteArray payload;
    };

    class RawMessageSourceRepository
    {
      public:
        explicit RawMessageSourceRepository(DatabaseConnection& connection);

        [[nodiscard]] std::optional<DatabaseError> upsert(std::string_view accountId,
                                                          const RawMessageSource& source);
        [[nodiscard]] std::optional<DatabaseError> upsertInstalled(std::string_view accountId,
                                                                   std::string_view emailId,
                                                                   std::string_view blobId,
                                                                   const MailVaultObject& object);
        [[nodiscard]] std::optional<DatabaseError> remove(std::string_view accountId,
                                                          std::string_view emailId);
        [[nodiscard]] std::variant<std::optional<RawMessageSource>, DatabaseError>
        find(std::string_view accountId, std::string_view emailId) const;
        [[nodiscard]] std::variant<std::optional<std::string>, DatabaseError>
        findBlobId(std::string_view accountId, std::string_view emailId) const;
        [[nodiscard]] std::variant<std::size_t, DatabaseError>
        migrateLegacySources(std::size_t limit = 25);
        [[nodiscard]] std::optional<DatabaseError> replayProjectionJobs(std::size_t limit = 100);
        [[nodiscard]] std::variant<std::size_t, DatabaseError>
        evictUnretained(std::size_t limit = 25);

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
