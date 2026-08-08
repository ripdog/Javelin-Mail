#include "gui/shell/TabPersistence.h"

#include <algorithm>
#include <utility>

namespace javelin::gui::shell
{
    namespace
    {
        [[nodiscard]] PersistedTabSelection persistSelection(const TabSelectionState& selection)
        {
            return {
                .threadId = selection.threadId,
                .emailId = selection.emailId,
            };
        }
    } // namespace

    PersistedMailboxTab persistMailboxTab(const MailboxTabPersistenceSnapshot& snapshot)
    {
        return {
            .common =
                {
                    .title = snapshot.title,
                    .selection = persistSelection(snapshot.selection),
                },
            .accountId = snapshot.accountId,
            .mailboxId = snapshot.mailboxId,
            .mailboxRole = snapshot.mailboxRole,
            .windows = snapshot.windows,
        };
    }

    PersistedSearchTab persistSearchTab(const SearchTabPersistenceSnapshot& snapshot)
    {
        return {
            .common =
                {
                    .title = snapshot.title,
                    .selection = persistSelection(snapshot.selection),
                },
            .accountId = snapshot.accountId,
            .search =
                {
                    .criteria = snapshot.criteria,
                    .restored =
                        {
                            .mode = snapshot.mode,
                            .sessionId = snapshot.sessionId,
                            .windows = snapshot.windows,
                        },
                },
        };
    }

    PersistedContactsTab persistContactsTab(const ContactsTabState& tab,
                                            javelin::gui::contacts::ContactsViewState view)
    {
        return {
            .common =
                {
                    .title = tab.title,
                    .selection = persistSelection(tab.selection),
                },
            .view = std::move(view),
        };
    }

    PersistedCalendarTab persistCalendarTab(const CalendarTabState& tab, QDate displayedMonth)
    {
        return {
            .common =
                {
                    .title = tab.title,
                    .selection = persistSelection(tab.selection),
                },
            .displayedMonth = std::move(displayedMonth),
        };
    }

    MailboxTabRestorePlan planMailboxTabRestore(const PersistedMailboxTab& tab)
    {
        return {
            .accountId = tab.accountId,
            .mailboxId = tab.mailboxId,
            .title = tab.common.title.isEmpty() ? QString::fromStdString(tab.mailboxId)
                                                : tab.common.title,
            .mailboxRole = tab.mailboxRole,
            .restored = {.windows = tab.windows},
            .selection =
                {
                    .threadId = tab.common.selection.threadId,
                    .emailId = tab.common.selection.emailId,
                    .selectedEmailIds = {},
                },
        };
    }

    SearchTabRestorePlan planSearchTabRestore(PersistedSearchTab tab)
    {
        return {
            .accountId = std::move(tab.accountId),
            .criteria = std::move(tab.search.criteria),
            .restored = std::move(tab.search.restored),
            .selection =
                {
                    .threadId = std::move(tab.common.selection.threadId),
                    .emailId = std::move(tab.common.selection.emailId),
                    .selectedEmailIds = {},
                },
        };
    }

    std::optional<int>
    resolveRestoredActiveTabIndex(const int persistedActiveTabIndex,
                                  const std::vector<std::optional<int>>& restoredTabIndices)
    {
        if (restoredTabIndices.empty())
            return std::nullopt;

        const auto persistedIndex =
            std::clamp(persistedActiveTabIndex, 0, static_cast<int>(restoredTabIndices.size() - 1));
        if (restoredTabIndices[static_cast<std::size_t>(persistedIndex)].has_value())
            return restoredTabIndices[static_cast<std::size_t>(persistedIndex)];

        for (std::size_t distance = 1; distance < restoredTabIndices.size(); ++distance)
        {
            const auto previous = persistedIndex - static_cast<int>(distance);
            if (previous >= 0 && restoredTabIndices[static_cast<std::size_t>(previous)].has_value())
                return restoredTabIndices[static_cast<std::size_t>(previous)];

            const auto next = persistedIndex + static_cast<int>(distance);
            if (next < static_cast<int>(restoredTabIndices.size()) &&
                restoredTabIndices[static_cast<std::size_t>(next)].has_value())
            {
                return restoredTabIndices[static_cast<std::size_t>(next)];
            }
        }

        return std::nullopt;
    }
} // namespace javelin::gui::shell
