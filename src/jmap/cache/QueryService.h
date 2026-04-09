#pragma once

#include "jmap/cache/Database.h"
#include "jmap/domain/MailEntities.h"

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
        bool hasChildren = false;
    };

    struct MessageListItem
    {
        std::string emailId;
        std::string threadId;
        std::optional<std::string> subject;
        std::optional<std::string> preview;
        std::string receivedAt;
        std::optional<std::string> sentAt;
        std::uint64_t threadMessageCount = 1;
        bool hasAttachment = false;
        bool isUnread = false;
        bool isFlagged = false;
        std::optional<javelin::jmap::domain::EmailAddress> from;
    };

    class QueryService
    {
      public:
        explicit QueryService(DatabaseConnection& connection);

        [[nodiscard]] std::variant<std::vector<MailboxTreeItem>, DatabaseError>
        listMailboxTree(std::string_view accountId) const;
        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        listMailboxMessages(std::string_view accountId, std::string_view mailboxId,
                            std::size_t limit, std::size_t offset = 0) const;
        [[nodiscard]] std::variant<std::size_t, DatabaseError>
        countMailboxMessages(std::string_view accountId, std::string_view mailboxId) const;
        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        listMessagesByEmailIds(std::string_view accountId,
                               const std::vector<std::string>& emailIds) const;
        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        listThreadMessages(std::string_view accountId, std::string_view threadId) const;

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
