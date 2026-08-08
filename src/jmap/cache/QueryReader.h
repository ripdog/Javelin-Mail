#pragma once

#include "jmap/cache/MailboxReadRepository.h"
#include "jmap/cache/QueryWindowCoverage.h"
#include "jmap/domain/MailEntities.h"
#include "jmap/query/EmailListSort.h"

#include <QString>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

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
        bool isJunk = false;
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
        QueryWindowCoverage coverage = QueryWindowCoverage::Server;
        QueryWindowMaterialization materialization = QueryWindowMaterialization::Complete;
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
        QueryWindowCoverage coverage = QueryWindowCoverage::Server;
        QueryWindowMaterialization materialization = QueryWindowMaterialization::Complete;
        std::vector<MessageListItem> items;
    };

    struct OfflineMailboxCoverage
    {
        std::uint64_t generation = 0;
        std::size_t representativeCount = 0;
        bool enumerationComplete = false;
    };

    class QueryReader
    {
      public:
        virtual ~QueryReader() = default;

        [[nodiscard]] virtual std::variant<std::vector<MailboxTreeItem>, DatabaseError>
        listMailboxTree(std::string_view accountId) const = 0;
        [[nodiscard]] virtual std::variant<std::vector<MessageListItem>, DatabaseError>
        listMailboxMessages(std::string_view accountId, std::string_view mailboxId,
                            std::size_t limit, std::size_t offset = 0,
                            javelin::jmap::query::EmailListSort sort = {}) const = 0;
        [[nodiscard]] virtual std::variant<std::optional<OfflineMailboxCoverage>, DatabaseError>
        offlineMailboxCoverage(std::string_view accountId, std::string_view mailboxId) const = 0;
        [[nodiscard]] virtual std::variant<std::optional<std::string>, DatabaseError>
        completeOfflineMailboxQueryState(std::string_view accountId, std::string_view mailboxId,
                                         std::string_view canonicalQueryKey) const = 0;
        [[nodiscard]] virtual std::variant<std::vector<MessageListItem>, DatabaseError>
        listOfflineMailboxMessages(std::string_view accountId, std::string_view mailboxId,
                                   std::uint64_t generation, std::size_t limit,
                                   std::size_t offset = 0,
                                   javelin::jmap::query::EmailListSort sort = {}) const = 0;
        [[nodiscard]] virtual std::variant<std::size_t, DatabaseError>
        countMailboxMessages(std::string_view accountId, std::string_view mailboxId) const = 0;
        [[nodiscard]] virtual std::variant<std::size_t, DatabaseError>
        countUnreadMailboxEmails(std::string_view accountId, std::string_view mailboxId) const = 0;
        [[nodiscard]] virtual std::variant<std::vector<MessageListItem>, DatabaseError>
        listMessagesByEmailIds(std::string_view accountId,
                               const std::vector<std::string>& emailIds) const = 0;
        [[nodiscard]] virtual std::variant<std::vector<MessageListItem>, DatabaseError>
        searchCachedMessageText(std::string_view accountId, std::string_view text,
                                std::size_t limit, std::size_t offset = 0) const = 0;
        [[nodiscard]] virtual std::variant<std::vector<MessageListItem>, DatabaseError>
        searchAllCachedMessageText(std::string_view accountId, std::string_view text,
                                   javelin::jmap::query::EmailListSort sort = {}) const = 0;
        [[nodiscard]] virtual QString databasePath() const = 0;
        [[nodiscard]] virtual std::variant<std::uint64_t, DatabaseError> dataVersion() const = 0;
        [[nodiscard]] virtual std::variant<std::optional<SearchWindowPage>, DatabaseError>
        loadSearchWindow(std::string_view accountId, std::string_view queryKey, std::size_t offset,
                         std::size_t limit) const = 0;
        [[nodiscard]] virtual std::variant<std::optional<MailboxWindowPage>, DatabaseError>
        loadMailboxWindow(std::string_view accountId, std::string_view queryKey,
                          std::size_t requestedOffset, std::size_t requestedLimit,
                          javelin::jmap::query::EmailListSort sort = {}) const = 0;
        [[nodiscard]] virtual std::variant<std::vector<MessageListItem>, DatabaseError>
        listThreadMessages(std::string_view accountId, std::string_view threadId) const = 0;
        [[nodiscard]] virtual std::variant<std::vector<MessageListItem>, DatabaseError>
        listMailboxThreadMessages(std::string_view accountId, std::string_view mailboxId,
                                  std::string_view threadId) const = 0;
    };

} // namespace javelin::jmap::cache
