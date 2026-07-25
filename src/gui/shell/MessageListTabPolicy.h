#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace javelin::gui::shell
{
    enum class MessageListTabCollection
    {
        Mailbox,
        Search,
    };

    struct MessageListTabIdentity
    {
        MessageListTabCollection collection = MessageListTabCollection::Mailbox;
        std::string accountId;
        std::string collectionKey;
    };

    [[nodiscard]] std::optional<std::size_t>
    findReusableMessageListTab(std::span<const std::optional<MessageListTabIdentity>> tabs,
                               const MessageListTabIdentity& requested, std::size_t firstIndex = 0);

    [[nodiscard]] std::vector<std::size_t>
    messageListTabsToMarkStale(std::span<const std::optional<MessageListTabIdentity>> tabs,
                               std::string_view accountId,
                               std::optional<std::string_view> refreshedMailboxId = std::nullopt,
                               bool searchesOnly = false);
} // namespace javelin::gui::shell
