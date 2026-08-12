#pragma once

#include "jmap/domain/MailEntities.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    struct MailboxTreeItem
    {
        std::string id;
        std::string name;
        std::optional<std::string> parentId;
        std::optional<std::string> role;
        std::uint64_t sortOrder = 0;
        std::uint64_t totalEmails = 0;
        std::uint64_t unreadEmails = 0;
        std::uint64_t totalThreads = 0;
        std::uint64_t unreadThreads = 0;
        bool isSubscribed = false;
        javelin::jmap::domain::MailboxRights myRights;
        bool hasChildren = false;
        bool pendingCreate = false;
    };

    class MailboxReader
    {
      public:
        virtual ~MailboxReader() = default;

        [[nodiscard]] virtual std::variant<std::vector<MailboxTreeItem>, DatabaseError>
        listMailboxTree(std::string_view accountId) const = 0;
    };

    class MailboxReadRepository final : public MailboxReader
    {
      public:
        explicit MailboxReadRepository(DatabaseConnection& connection);
        explicit MailboxReadRepository(ReadOnlyDatabaseConnection& connection);

        [[nodiscard]] std::variant<std::vector<MailboxTreeItem>, DatabaseError>
        listMailboxTree(std::string_view accountId) const override;

      private:
        DatabaseReadView m_connection;
    };

} // namespace javelin::jmap::cache
