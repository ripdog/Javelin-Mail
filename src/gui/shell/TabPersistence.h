#pragma once

#include "app/MailboxSession.h"
#include "app/SearchSession.h"
#include "gui/shell/MainWindowStateStore.h"
#include "gui/shell/TabWorkspace.h"

#include <QString>

#include <cstddef>
#include <optional>
#include <string>

namespace javelin::gui::shell
{
    struct MailboxTabPersistenceSnapshot
    {
        std::string accountId;
        QString title;
        TabSelectionState selection;
        std::string mailboxId;
        std::optional<std::string> mailboxRole;
        std::size_t offset = 0;
    };

    struct SearchTabPersistenceSnapshot
    {
        std::string accountId;
        QString title;
        TabSelectionState selection;
        javelin::jmap::search::EmailSearchCriteria criteria;
        javelin::app::MessageListPage page;
        javelin::app::SearchMode mode = javelin::app::SearchMode::Local;
        std::string sessionId;
    };

    struct MailboxTabRestorePlan
    {
        std::string accountId;
        std::string mailboxId;
        QString title;
        std::optional<std::string> mailboxRole;
        javelin::app::RestoredMailboxState restored;
        TabSelectionState selection;
    };

    struct SearchTabRestorePlan
    {
        std::string accountId;
        javelin::jmap::search::EmailSearchCriteria criteria;
        javelin::app::RestoredSearchState restored;
        TabSelectionState selection;
    };

    [[nodiscard]] PersistedMailboxTab
    persistMailboxTab(const MailboxTabPersistenceSnapshot& snapshot);
    [[nodiscard]] PersistedSearchTab persistSearchTab(const SearchTabPersistenceSnapshot& snapshot);
    [[nodiscard]] PersistedTab persistTab(const TabState& tab);

    [[nodiscard]] MailboxTabRestorePlan planMailboxTabRestore(const PersistedMailboxTab& tab,
                                                              std::size_t pageSize);
    [[nodiscard]] SearchTabRestorePlan planSearchTabRestore(PersistedSearchTab tab);
} // namespace javelin::gui::shell
