#pragma once

#include "jmap/cache/MailVault.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QByteArray>

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace javelin::jmap::cache
{

    struct RawMessageSource
    {
        std::string emailId;
        std::string blobId;
        QByteArray payload;
    };

    struct RawMessageSourceReference
    {
        std::string emailId;
        std::string blobId;
        MailVaultObject object;
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
        [[nodiscard]] std::variant<bool, DatabaseError>
        upsertInstalledIfCurrent(std::string_view accountId, std::string_view emailId,
                                 std::string_view blobId, const MailVaultObject& object);
        [[nodiscard]] std::optional<DatabaseError> remove(std::string_view accountId,
                                                          std::string_view emailId);
        [[nodiscard]] std::variant<std::optional<RawMessageSource>, DatabaseError>
        find(std::string_view accountId, std::string_view emailId) const;
        [[nodiscard]] std::variant<std::optional<RawMessageSourceReference>, DatabaseError>
        findReference(std::string_view accountId, std::string_view emailId);
        [[nodiscard]] std::variant<std::optional<MailVaultObject>, DatabaseError>
        findVaultObject(std::string_view contentHash) const;
        [[nodiscard]] std::variant<std::optional<std::string>, DatabaseError>
        findBlobId(std::string_view accountId, std::string_view emailId) const;
        [[nodiscard]] std::variant<std::size_t, DatabaseError>
        migrateLegacySources(std::size_t limit = 25);
        [[nodiscard]] std::optional<DatabaseError> replayProjectionJobs(std::size_t limit = 100);
        [[nodiscard]] std::optional<DatabaseError>
        replayProjectionJobsForMailbox(std::string_view accountId, std::string_view mailboxId,
                                       std::size_t limit = 100);
        [[nodiscard]] std::variant<std::size_t, DatabaseError>
        evictUnretained(std::size_t limit = 25);

      private:
        [[nodiscard]] std::variant<bool, DatabaseError>
        upsertInstalledImpl(std::string_view accountId, std::string_view emailId,
                            std::string_view blobId, const MailVaultObject& object,
                            bool requireCurrentEmail);

        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
