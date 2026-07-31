#pragma once

#include "jmap/cache/QueryService.h"

#include <QString>

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace javelin::app
{
    using MessageListThreadMembersResult =
        std::variant<std::vector<javelin::jmap::cache::MessageListItem>,
                     javelin::jmap::cache::DatabaseError>;

    [[nodiscard]] inline MessageListThreadMembersResult
    loadMessageListThreadMembers(const QString& databasePath, const std::string& accountId,
                                 const std::optional<std::string>& mailboxId,
                                 const std::string& threadId)
    {
        javelin::jmap::cache::ReadOnlyThreadConnectionFactory factory{
            {.connectionNamePrefix = QStringLiteral("javelin-thread-members"),
             .databasePath = databasePath}};
        auto connectionResult = factory.openForCurrentThread("snapshot");
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&connectionResult))
        {
            return *error;
        }
        auto connection =
            std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(std::move(connectionResult));
        javelin::jmap::cache::QueryService queryService{connection};
        return mailboxId.has_value()
                   ? queryService.listMailboxThreadMessages(accountId, *mailboxId, threadId)
                   : queryService.listThreadMessages(accountId, threadId);
    }
} // namespace javelin::app
