#include "gui/shell/TabPersistence.h"

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
                    .accountId = snapshot.accountId,
                    .title = snapshot.title,
                    .selection = persistSelection(snapshot.selection),
                },
            .mailboxId = snapshot.mailboxId,
            .mailboxRole = snapshot.mailboxRole,
            .offset = snapshot.offset,
        };
    }

    PersistedSearchTab persistSearchTab(const SearchTabPersistenceSnapshot& snapshot)
    {
        return {
            .common =
                {
                    .accountId = snapshot.accountId,
                    .title = snapshot.title,
                    .selection = persistSelection(snapshot.selection),
                },
            .search =
                {
                    .criteria = snapshot.criteria,
                    .restored =
                        {
                            .page = snapshot.page,
                            .mode = snapshot.mode,
                            .sessionId = snapshot.sessionId,
                        },
                },
        };
    }

    MailboxTabRestorePlan planMailboxTabRestore(const PersistedMailboxTab& tab,
                                                const std::size_t pageSize)
    {
        return {
            .accountId = tab.common.accountId,
            .mailboxId = tab.mailboxId,
            .title = tab.common.title.isEmpty() ? QString::fromStdString(tab.mailboxId)
                                                : tab.common.title,
            .mailboxRole = tab.mailboxRole,
            .restored =
                {
                    .page =
                        {
                            .offset = tab.offset,
                            .installedOffset = std::nullopt,
                            .pendingOffset = std::nullopt,
                            .position = tab.offset,
                            .returnedLimit = pageSize,
                            .total = std::nullopt,
                            .queryState = {},
                            .anchor = std::nullopt,
                            .items = {},
                            .cacheLoaded = false,
                            .refreshInFlight = false,
                            .stale = false,
                            .refreshError = {},
                        },
                },
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
            .accountId = std::move(tab.common.accountId),
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
} // namespace javelin::gui::shell
