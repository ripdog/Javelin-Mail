#pragma once

#include "app/MailboxSession.h"
#include "app/SearchSession.h"
#include "gui/shell/MainWindowStateStore.h"
#include "gui/shell/TabWorkspace.h"

#include <QString>

#include <optional>
#include <string>
#include <vector>

namespace javelin::gui::shell
{
    struct MailboxTabPersistenceSnapshot
    {
        std::string accountId;
        QString title;
        TabSelectionState selection;
        std::string mailboxId;
        std::optional<std::string> mailboxRole;
        std::vector<javelin::app::MessageListWindowRequest> windows;
    };

    struct SearchTabPersistenceSnapshot
    {
        std::string accountId;
        QString title;
        TabSelectionState selection;
        javelin::jmap::search::EmailSearchCriteria criteria;
        javelin::app::SearchMode mode = javelin::app::SearchMode::Local;
        std::string sessionId;
        std::vector<javelin::app::MessageListWindowRequest> windows;
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
    [[nodiscard]] PersistedContactsTab
    persistContactsTab(const ContactsTabState& tab, javelin::gui::contacts::ContactsViewState view);
    [[nodiscard]] PersistedCalendarTab persistCalendarTab(const CalendarTabState& tab,
                                                          QDate displayedMonth);
    [[nodiscard]] PersistedTab persistTab(const TabState& tab);

    [[nodiscard]] MailboxTabRestorePlan planMailboxTabRestore(const PersistedMailboxTab& tab);
    [[nodiscard]] SearchTabRestorePlan planSearchTabRestore(PersistedSearchTab tab);
    [[nodiscard]] std::optional<int>
    resolveRestoredActiveTabIndex(int persistedActiveTabIndex,
                                  const std::vector<std::optional<int>>& restoredTabIndices);
} // namespace javelin::gui::shell
