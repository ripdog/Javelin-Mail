#pragma once

#include "jmap/cache/Database.h"
#include "jmap/cache/QueryReader.h"
#include "jmap/search/EmailSearch.h"

#include <optional>

namespace javelin::jmap::cache
{

    class QueryService final : public QueryReader
    {
      public:
        explicit QueryService(DatabaseConnection& connection);
        explicit QueryService(ReadOnlyDatabaseConnection& connection);

        [[nodiscard]] std::variant<std::vector<MailboxTreeItem>, DatabaseError>
        listMailboxTree(std::string_view accountId) const override;
        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        listMailboxMessages(std::string_view accountId, std::string_view mailboxId,
                            std::size_t limit, std::size_t offset = 0,
                            javelin::jmap::query::EmailListSort sort = {}) const override;
        [[nodiscard]] std::variant<std::optional<OfflineMailboxCoverage>, DatabaseError>
        offlineMailboxCoverage(std::string_view accountId,
                               std::string_view mailboxId) const override;
        [[nodiscard]] std::variant<std::optional<std::string>, DatabaseError>
        completeOfflineMailboxQueryState(std::string_view accountId, std::string_view mailboxId,
                                         std::string_view canonicalQueryKey) const override;
        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        listOfflineMailboxMessages(std::string_view accountId, std::string_view mailboxId,
                                   std::uint64_t generation, std::size_t limit,
                                   std::size_t offset = 0,
                                   javelin::jmap::query::EmailListSort sort = {}) const override;
        [[nodiscard]] std::variant<std::vector<std::string>, DatabaseError>
        listOfflineMailboxRepresentativeIds(std::string_view accountId, std::string_view mailboxId,
                                            std::uint64_t generation, std::size_t limit,
                                            std::size_t offset = 0,
                                            javelin::jmap::query::EmailListSort sort = {}) const;
        [[nodiscard]] std::variant<std::size_t, DatabaseError>
        countMailboxMessages(std::string_view accountId, std::string_view mailboxId) const override;
        [[nodiscard]] std::variant<std::size_t, DatabaseError>
        countUnreadMailboxEmails(std::string_view accountId,
                                 std::string_view mailboxId) const override;
        [[nodiscard]] std::variant<std::vector<std::string>, DatabaseError>
        listUserKeywords(std::string_view accountId,
                         std::string_view mailboxId = {}) const override;
        [[nodiscard]] std::variant<std::vector<std::string>, DatabaseError>
        listTagKeywords(std::string_view accountId) const override;
        [[nodiscard]] std::variant<std::vector<EmailKeywordMembership>, DatabaseError>
        listEmailKeywordMemberships(std::string_view accountId,
                                    const std::vector<std::string>& emailIds) const override;
        [[nodiscard]] std::variant<std::vector<TagDefinition>, DatabaseError>
        listTagDefinitions(std::string_view accountId) const override;
        [[nodiscard]] std::variant<std::vector<std::string>, DatabaseError>
        listContactEmailAddresses() const;
        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        listFilteredMailboxMessages(std::string_view accountId, std::string_view mailboxId,
                                    const javelin::jmap::search::EmailSearchCriteria& criteria,
                                    std::size_t limit, std::size_t offset = 0,
                                    javelin::jmap::query::EmailListSort sort = {}) const;
        [[nodiscard]] std::variant<std::size_t, DatabaseError> countFilteredMailboxMessages(
            std::string_view accountId, std::string_view mailboxId,
            const javelin::jmap::search::EmailSearchCriteria& criteria) const;
        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        listMessagesByEmailIds(std::string_view accountId,
                               const std::vector<std::string>& emailIds) const override;
        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        searchCachedMessageText(std::string_view accountId, std::string_view text,
                                std::size_t limit, std::size_t offset = 0) const override;
        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        searchAllCachedMessageText(std::string_view accountId, std::string_view text,
                                   javelin::jmap::query::EmailListSort sort = {}) const override;
        [[nodiscard]] QString databasePath() const override;
        [[nodiscard]] std::variant<std::uint64_t, DatabaseError> dataVersion() const override;
        [[nodiscard]] std::variant<std::optional<SearchWindowPage>, DatabaseError>
        loadSearchWindow(std::string_view accountId, std::string_view queryKey, std::size_t offset,
                         std::size_t limit) const override;
        [[nodiscard]] std::optional<DatabaseError>
        eraseSearchWindows(std::string_view accountId, std::string_view queryKey) const;
        [[nodiscard]] std::variant<std::optional<MailboxWindowPage>, DatabaseError>
        loadMailboxWindow(std::string_view accountId, std::string_view queryKey,
                          std::size_t requestedOffset, std::size_t requestedLimit,
                          javelin::jmap::query::EmailListSort sort = {}) const override;
        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        listThreadMessages(std::string_view accountId, std::string_view threadId) const override;
        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        listMailboxThreadMessages(std::string_view accountId, std::string_view mailboxId,
                                  std::string_view threadId) const override;

      private:
        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        listMailboxWindowMessagesByEmailIds(std::string_view accountId, std::string_view mailboxId,
                                            const std::vector<std::string>& emailIds,
                                            javelin::jmap::query::EmailListSort sort) const;
        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        listSortedSearchMessagesByEmailIds(std::string_view accountId,
                                           const std::vector<std::string>& emailIds,
                                           javelin::jmap::query::EmailListSort sort) const;

        DatabaseReadView m_connection;
        DatabaseConnection* m_writeConnection = nullptr;
    };

} // namespace javelin::jmap::cache
