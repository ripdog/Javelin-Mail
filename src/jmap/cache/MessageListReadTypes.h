#pragma once

#include "jmap/cache/QueryWindowCoverage.h"
#include "jmap/domain/MailEntities.h"

#include <QString>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace javelin::jmap::cache
{
    struct MessageListTag
    {
        std::string keyword;
        QString displayName;
        QString color;

        friend bool operator==(const MessageListTag&, const MessageListTag&) = default;
    };

    struct MessageListItem
    {
        std::string emailId;
        std::string threadId;
        std::optional<std::string> subject;
        std::optional<std::string> preview;
        std::optional<std::string> bodyPreview = std::nullopt;
        std::string receivedAt;
        std::optional<std::string> sentAt;
        std::optional<std::uint64_t> mailboxThreadMessageCount = std::nullopt;
        std::optional<std::uint64_t> globalThreadMessageCount = std::nullopt;
        bool hasAttachment = false;
        bool isUnread = false;
        bool isFlagged = false;
        bool isJunk = false;
        std::optional<javelin::jmap::domain::EmailAddress> from;
        std::vector<std::string> mailboxNames;
        std::vector<MessageListTag> tags{};
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

    enum class MailboxThreadMembershipSource
    {
        NormalizedThread,
        CompleteOfflineMailbox,
    };

} // namespace javelin::jmap::cache
