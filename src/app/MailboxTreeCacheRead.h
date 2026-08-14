#pragma once

#include "storage/sqlite/DatabaseConnection.h"

#include "jmap/cache/AccountReadRepository.h"
#include "jmap/cache/MailboxReadRepository.h"

#include <QString>

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace javelin::app
{
    struct MailboxTreeCacheSnapshot
    {
        std::vector<javelin::jmap::cache::CachedAccount> accounts;
        std::unordered_map<std::string, std::vector<javelin::jmap::cache::MailboxTreeItem>>
            mailboxesByAccount;
    };

    using MailboxTreeCacheReadResult =
        std::variant<MailboxTreeCacheSnapshot, javelin::jmap::cache::DatabaseError>;

    [[nodiscard]] inline MailboxTreeCacheReadResult
    loadMailboxTreeCache(const QString& databasePath,
                         const std::optional<std::string>& accountId = std::nullopt)
    {
        javelin::jmap::cache::ReadOnlyThreadConnectionFactory factory{
            {.connectionNamePrefix = QStringLiteral("javelin-mailbox-tree"),
             .databasePath = databasePath}};
        auto connectionResult = factory.openForCurrentThread("snapshot");
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&connectionResult))
        {
            return *error;
        }
        auto connection =
            std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(std::move(connectionResult));
        javelin::jmap::cache::AccountReadRepository accountReader{connection};
        javelin::jmap::cache::MailboxReadRepository mailboxReader{connection};
        auto accountsResult = accountReader.listAll();
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&accountsResult))
        {
            return *error;
        }

        const auto& cachedAccounts =
            std::get<std::vector<javelin::jmap::cache::CachedAccount>>(accountsResult);
        MailboxTreeCacheSnapshot snapshot;
        snapshot.accounts.reserve(cachedAccounts.size());
        for (const auto& account : cachedAccounts)
        {
            if (!account.hasMailCapability)
                continue;
            snapshot.accounts.push_back(account);
            if (accountId.has_value() && account.accountId != *accountId)
                continue;
            auto mailboxesResult = mailboxReader.listMailboxTree(account.accountId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&mailboxesResult))
            {
                return *error;
            }
            snapshot.mailboxesByAccount.emplace(
                account.accountId, std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(
                                       std::move(mailboxesResult)));
        }
        return snapshot;
    }
} // namespace javelin::app
