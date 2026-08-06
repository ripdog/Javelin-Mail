#pragma once

#include "jmap/cache/Database.h"
#include "jmap/cache/IdentityReader.h"
#include "jmap/domain/MailEntities.h"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    class IdentityRepository final : public IdentityReader
    {
      public:
        explicit IdentityRepository(DatabaseConnection& connection);
        explicit IdentityRepository(ReadOnlyDatabaseConnection& connection);

        [[nodiscard]] std::optional<DatabaseError>
        replaceAll(std::string_view accountId,
                   const std::vector<javelin::jmap::domain::Identity>& identities,
                   std::string_view state = {});
        [[nodiscard]] std::optional<DatabaseError>
        replaceAll(DatabaseTransaction& transaction, std::string_view accountId,
                   const std::vector<javelin::jmap::domain::Identity>& identities,
                   std::string_view state);
        [[nodiscard]] std::optional<DatabaseError>
        applyChanges(std::string_view accountId,
                     const std::vector<javelin::jmap::domain::Identity>& upserts,
                     const std::vector<std::string>& destroyedIds, std::string_view state);
        [[nodiscard]] std::optional<DatabaseError>
        projectUpsert(DatabaseTransaction& transaction, std::string_view accountId,
                      const javelin::jmap::domain::Identity& identity);
        [[nodiscard]] std::optional<DatabaseError> projectDestroy(DatabaseTransaction& transaction,
                                                                  std::string_view accountId,
                                                                  std::string_view identityId);
        [[nodiscard]] std::optional<DatabaseError>
        projectPendingCreate(DatabaseTransaction& transaction, std::string_view accountId,
                             std::string_view creationId, std::string_view mutationId,
                             const javelin::jmap::domain::Identity& identity);
        [[nodiscard]] std::optional<DatabaseError>
        removePendingCreate(DatabaseTransaction& transaction, std::string_view accountId,
                            std::string_view creationId);
        [[nodiscard]] std::optional<DatabaseError>
        removeAllPendingCreates(DatabaseTransaction& transaction, std::string_view accountId);

        [[nodiscard]] std::variant<std::vector<javelin::jmap::domain::Identity>, DatabaseError>
        listByAccount(std::string_view accountId) const override;
        [[nodiscard]] std::variant<std::optional<javelin::jmap::domain::Identity>, DatabaseError>
        find(std::string_view accountId, std::string_view identityId) const override;
        [[nodiscard]] std::variant<std::vector<PendingIdentityCreate>, DatabaseError>
        listPendingCreates(std::string_view accountId) const override;
        [[nodiscard]] std::variant<std::optional<std::string>, DatabaseError>
        state(std::string_view accountId) const;

      private:
        [[nodiscard]] std::optional<DatabaseError>
        requireWritableTransaction(const DatabaseTransaction& transaction, QString operation) const;

        DatabaseReadView m_connection;
        DatabaseConnection* m_writeConnection = nullptr;
    };

} // namespace javelin::jmap::cache
