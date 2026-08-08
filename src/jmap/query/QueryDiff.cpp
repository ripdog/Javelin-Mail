#include "jmap/query/QueryDiff.h"

#include <algorithm>
#include <unordered_map>

namespace javelin::jmap::query
{
    std::optional<std::size_t> selectionFallbackIndexAfterRemoval(const std::size_t previousIndex,
                                                                  const std::size_t currentCount)
    {
        if (currentCount == 0)
        {
            return std::nullopt;
        }

        return std::min(previousIndex, currentCount - 1);
    }

    namespace
    {

        [[nodiscard]] bool equalEmailAddress(const javelin::jmap::domain::EmailAddress& left,
                                             const javelin::jmap::domain::EmailAddress& right)
        {
            return left.name == right.name && left.email == right.email;
        }

        [[nodiscard]] bool equalItems(const javelin::jmap::cache::MailboxTreeItem& left,
                                      const javelin::jmap::cache::MailboxTreeItem& right)
        {
            const auto& leftRights = left.myRights;
            const auto& rightRights = right.myRights;
            return left.id == right.id && left.name == right.name &&
                   left.parentId == right.parentId && left.role == right.role &&
                   left.sortOrder == right.sortOrder && left.totalEmails == right.totalEmails &&
                   left.unreadEmails == right.unreadEmails &&
                   left.totalThreads == right.totalThreads &&
                   left.unreadThreads == right.unreadThreads &&
                   left.isSubscribed == right.isSubscribed &&
                   leftRights.mayReadItems == rightRights.mayReadItems &&
                   leftRights.mayAddItems == rightRights.mayAddItems &&
                   leftRights.mayRemoveItems == rightRights.mayRemoveItems &&
                   leftRights.maySetSeen == rightRights.maySetSeen &&
                   leftRights.maySetKeywords == rightRights.maySetKeywords &&
                   leftRights.mayCreateChild == rightRights.mayCreateChild &&
                   leftRights.mayRename == rightRights.mayRename &&
                   leftRights.mayDelete == rightRights.mayDelete &&
                   leftRights.maySubmit == rightRights.maySubmit &&
                   left.hasChildren == right.hasChildren;
        }

        [[nodiscard]] bool equalItems(const javelin::jmap::cache::MessageListItem& left,
                                      const javelin::jmap::cache::MessageListItem& right)
        {
            const auto equalFrom = (!left.from.has_value() && !right.from.has_value()) ||
                                   (left.from.has_value() && right.from.has_value() &&
                                    equalEmailAddress(*left.from, *right.from));
            return left.emailId == right.emailId && left.threadId == right.threadId &&
                   left.subject == right.subject && left.preview == right.preview &&
                   left.receivedAt == right.receivedAt && left.sentAt == right.sentAt &&
                   left.hasAttachment == right.hasAttachment && left.isUnread == right.isUnread &&
                   left.isFlagged == right.isFlagged && left.mailboxNames == right.mailboxNames &&
                   left.tags == right.tags && equalFrom;
        }

        [[nodiscard]] MailboxSelectionKey makeSelectionKey(const std::string& id,
                                                           const MailboxSelectionKey*)
        {
            return MailboxSelectionKey{.mailboxId = id};
        }

        [[nodiscard]] MessageSelectionKey makeSelectionKey(const std::string& id,
                                                           const MessageSelectionKey*)
        {
            return MessageSelectionKey{.threadId = id};
        }

        template <typename Item, typename SelectionKey, typename Refresh, typename MakeKey>
        [[nodiscard]] Refresh
        diffItems(const std::vector<Item>& previous, const std::vector<Item>& current,
                  const std::optional<SelectionKey>& currentSelection, MakeKey&& makeKey)
        {
            Refresh refresh;

            std::unordered_map<std::string, std::size_t> previousIndexes;
            previousIndexes.reserve(previous.size());
            for (std::size_t index = 0; index < previous.size(); ++index)
            {
                previousIndexes.emplace(makeKey(previous[index]), index);
            }

            std::unordered_map<std::string, std::size_t> currentIndexes;
            currentIndexes.reserve(current.size());
            for (std::size_t index = 0; index < current.size(); ++index)
            {
                currentIndexes.emplace(makeKey(current[index]), index);
            }

            for (std::size_t index = previous.size(); index > 0; --index)
            {
                const auto& item = previous[index - 1];
                const auto key = makeKey(item);
                if (!currentIndexes.contains(key))
                {
                    refresh.changes.push_back({
                        .kind = QueryRowChangeKind::Remove,
                        .key = makeSelectionKey(key, static_cast<const SelectionKey*>(nullptr)),
                        .oldIndex = index - 1,
                        .newIndex = std::nullopt,
                    });
                }
            }

            for (std::size_t index = 0; index < current.size(); ++index)
            {
                const auto& item = current[index];
                const auto key = makeKey(item);
                const auto previousIt = previousIndexes.find(key);
                if (previousIt == previousIndexes.end())
                {
                    refresh.changes.push_back({
                        .kind = QueryRowChangeKind::Insert,
                        .key = makeSelectionKey(key, static_cast<const SelectionKey*>(nullptr)),
                        .oldIndex = std::nullopt,
                        .newIndex = index,
                    });
                    continue;
                }

                const auto previousIndex = previousIt->second;
                if (previousIndex != index)
                {
                    refresh.changes.push_back({
                        .kind = QueryRowChangeKind::Move,
                        .key = makeSelectionKey(key, static_cast<const SelectionKey*>(nullptr)),
                        .oldIndex = previousIndex,
                        .newIndex = index,
                    });
                }

                if (!equalItems(previous[previousIndex], item))
                {
                    refresh.changes.push_back({
                        .kind = QueryRowChangeKind::Update,
                        .key = makeSelectionKey(key, static_cast<const SelectionKey*>(nullptr)),
                        .oldIndex = previousIndex,
                        .newIndex = index,
                    });
                }
            }

            if (currentSelection.has_value())
            {
                const auto selectedKey = [&currentSelection]()
                {
                    if constexpr (std::is_same_v<SelectionKey, MailboxSelectionKey>)
                    {
                        return currentSelection->mailboxId;
                    }
                    else
                    {
                        return currentSelection->threadId;
                    }
                }();

                if (currentIndexes.contains(selectedKey))
                {
                    refresh.selectionPreserved = true;
                    refresh.nextSelection = currentSelection;
                }
            }

            if (!refresh.nextSelection.has_value() && !current.empty())
            {
                const auto key = makeKey(current.front());
                refresh.nextSelection =
                    makeSelectionKey(key, static_cast<const SelectionKey*>(nullptr));
            }

            return refresh;
        }

    } // namespace

    MailboxTreeRefresh
    diffMailboxTree(const std::vector<javelin::jmap::cache::MailboxTreeItem>& previous,
                    const std::vector<javelin::jmap::cache::MailboxTreeItem>& current,
                    const std::optional<MailboxSelectionKey> currentSelection)
    {
        return diffItems<javelin::jmap::cache::MailboxTreeItem, MailboxSelectionKey,
                         MailboxTreeRefresh>(previous, current, currentSelection,
                                             [](const auto& item) { return item.id; });
    }

    MessageListRefresh
    diffMessageList(const std::vector<javelin::jmap::cache::MessageListItem>& previous,
                    const std::vector<javelin::jmap::cache::MessageListItem>& current,
                    const std::optional<MessageSelectionKey> currentSelection)
    {
        return diffItems<javelin::jmap::cache::MessageListItem, MessageSelectionKey,
                         MessageListRefresh>(previous, current, currentSelection,
                                             [](const auto& item) { return item.threadId; });
    }

} // namespace javelin::jmap::query
