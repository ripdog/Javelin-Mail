#pragma once

#include "jmap/cache/Database.h"
#include "jmap/domain/MailEntities.h"
#include "jmap/query/EmailListSort.h"

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
        std::vector<std::string> mailboxNames;
    };

    struct SearchWindowPage
    {
        std::size_t offset = 0;
        std::size_t limit = 0;
        std::size_t position = 0;
        std::size_t returnedLimit = 0;
        std::optional<std::size_t> total;
        std::string queryState;
        std::vector<MessageListItem> items;
    };

    struct MailboxWindowPage
    {
        std::size_t requestedOffset = 0;
        std::size_t requestedLimit = 0;
        std::size_t position = 0;
        std::size_t returnedLimit = 0;
        std::optional<std::size_t> total;
        std::string queryState;
        std::vector<MessageListItem> items;
    };

    class QueryService
    {
      public:
        explicit QueryService(DatabaseConnection& connection);

        [[nodiscard]] std::variant<std::vector<MailboxTreeItem>, DatabaseError>
        listMailboxTree(std::string_view accountId) const;
        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        listMailboxMessages(std::string_view accountId, std::string_view mailboxId,
                            std::size_t limit, std::size_t offset = 0,
                            javelin::jmap::query::EmailListSort sort = {}) const;
        [[nodiscard]] std::variant<std::size_t, DatabaseError>
        countMailboxMessages(std::string_view accountId, std::string_view mailboxId) const;
        [[nodiscard]] std::variant<std::size_t, DatabaseError>
        countUnreadMailboxEmails(std::string_view accountId, std::string_view mailboxId) const;
        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        listMessagesByEmailIds(std::string_view accountId,
                               const std::vector<std::string>& emailIds) const;
        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        searchCachedMessageText(std::string_view accountId, std::string_view text,
                                std::size_t limit, std::size_t offset = 0) const;
        [[nodiscard]] QString databasePath() const;
        [[nodiscard]] std::variant<std::optional<SearchWindowPage>, DatabaseError>
        loadSearchWindow(std::string_view accountId, std::string_view queryKey, std::size_t offset,
                         std::size_t limit) const;
        [[nodiscard]] std::variant<std::optional<MailboxWindowPage>, DatabaseError>
        loadMailboxWindow(std::string_view accountId, std::string_view queryKey,
                          std::size_t requestedOffset, std::size_t requestedLimit) const;
        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        listThreadMessages(std::string_view accountId, std::string_view threadId) const;
        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        listMailboxThreadMessages(std::string_view accountId, std::string_view mailboxId,
                                  std::string_view threadId) const;

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
