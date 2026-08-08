#pragma once

#include "gui/contacts/ContactsViewState.h"
#include "gui/search/SearchSessionPersistence.h"
#include "jmap/query/EmailListSort.h"

#include <QByteArray>
#include <QDate>
#include <QString>

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace javelin::gui::shell
{
    struct PersistedTabSelection
    {
        std::optional<std::string> threadId;
        std::optional<std::string> emailId;
    };

    struct PersistedTabCommon
    {
        QString title;
        PersistedTabSelection selection;
    };

    struct PersistedMailboxTab
    {
        PersistedTabCommon common;
        std::string accountId;
        std::string mailboxId;
        std::optional<std::string> mailboxRole;
        std::vector<javelin::app::MessageListWindowRequest> windows;
    };

    struct PersistedSearchTab
    {
        PersistedTabCommon common;
        std::string accountId;
        javelin::gui::search::PersistedSearchState search;
    };

    struct PersistedComposeTab
    {
        PersistedTabCommon common;
        std::string accountId;
        std::string composeSessionId;
        bool hasUnsavedChanges = false;
    };

    struct PersistedContactsTab
    {
        PersistedTabCommon common;
        javelin::gui::contacts::ContactsViewState view;
    };

    struct PersistedCalendarTab
    {
        PersistedTabCommon common;
        QDate displayedMonth;
    };

    using PersistedTab = std::variant<PersistedMailboxTab, PersistedSearchTab, PersistedComposeTab,
                                      PersistedContactsTab, PersistedCalendarTab>;

    struct PersistedMainWindowState
    {
        QByteArray geometry;
        QByteArray splitterState;
        int activeTabIndex = 0;
        javelin::jmap::query::EmailListSort emailListSort;
        std::vector<PersistedTab> tabs;
    };

    [[nodiscard]] QByteArray serializeMainWindowState(const PersistedMainWindowState& state);
    [[nodiscard]] PersistedMainWindowState
    deserializeMainWindowState(const QByteArray& encoded,
                               javelin::jmap::query::EmailListSort defaultSort);
} // namespace javelin::gui::shell
