#pragma once

#include "jmap/cache/QueryService.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace javelin::jmap::query
{

    struct MailboxSelectionKey
    {
        std::string mailboxId;
    };

    struct MessageSelectionKey
    {
        std::string emailId;
    };

    enum class QueryRowChangeKind
    {
        Insert,
        Remove,
        Move,
        Update,
    };

    template <typename SelectionKey> struct QueryRowChange
    {
        QueryRowChangeKind kind = QueryRowChangeKind::Update;
        SelectionKey key;
        std::optional<std::size_t> oldIndex;
        std::optional<std::size_t> newIndex;
    };

    struct MailboxTreeRefresh
    {
        std::vector<QueryRowChange<MailboxSelectionKey>> changes;
        std::optional<MailboxSelectionKey> nextSelection;
        bool selectionPreserved = false;
    };

    struct MessageListRefresh
    {
        std::vector<QueryRowChange<MessageSelectionKey>> changes;
        std::optional<MessageSelectionKey> nextSelection;
        bool selectionPreserved = false;
    };

    [[nodiscard]] MailboxTreeRefresh
    diffMailboxTree(const std::vector<javelin::jmap::cache::MailboxTreeItem>& previous,
                    const std::vector<javelin::jmap::cache::MailboxTreeItem>& current,
                    std::optional<MailboxSelectionKey> currentSelection);

    [[nodiscard]] MessageListRefresh
    diffMessageList(const std::vector<javelin::jmap::cache::MessageListItem>& previous,
                    const std::vector<javelin::jmap::cache::MessageListItem>& current,
                    std::optional<MessageSelectionKey> currentSelection);

} // namespace javelin::jmap::query
