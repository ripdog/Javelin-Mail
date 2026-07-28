#pragma once

#include "jmap/cache/Database.h"
#include "jmap/domain/MailEntities.h"

#include <QString>

#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    [[nodiscard]] javelin::jmap::domain::MailboxRights
    deserializeMailboxRights(const QString& json);

    class MailboxRepository
    {
      public:
        explicit MailboxRepository(DatabaseConnection& connection);

        [[nodiscard]] std::optional<DatabaseError>
        replaceAll(std::string_view accountId,
                   const std::vector<javelin::jmap::domain::Mailbox>& mailboxes);
        [[nodiscard]] std::optional<DatabaseError>
        replaceAll(DatabaseTransaction& transaction, std::string_view accountId,
                   const std::vector<javelin::jmap::domain::Mailbox>& mailboxes);
        [[nodiscard]] std::optional<DatabaseError>
        upsertMany(std::string_view accountId,
                   const std::vector<javelin::jmap::domain::Mailbox>& mailboxes);
        [[nodiscard]] std::optional<DatabaseError>
        upsertMany(DatabaseTransaction& transaction, std::string_view accountId,
                   const std::vector<javelin::jmap::domain::Mailbox>& mailboxes);
        [[nodiscard]] std::optional<DatabaseError>
        removeMany(std::string_view accountId, std::span<const std::string> mailboxIds);
        [[nodiscard]] std::optional<DatabaseError>
        removeMany(DatabaseTransaction& transaction, std::string_view accountId,
                   std::span<const std::string> mailboxIds);
        [[nodiscard]] std::variant<std::vector<javelin::jmap::domain::Mailbox>, DatabaseError>
        listByParent(std::string_view accountId, std::optional<std::string_view> parentId) const;

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
