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

class QSettings;

namespace javelin::gui::shell
{
    struct PersistedTabSelection
    {
        std::optional<std::string> threadId;
        std::optional<std::string> emailId;
    };

    struct PersistedTabCommon
    {
        std::string accountId;
        QString title;
        PersistedTabSelection selection;
    };

    struct PersistedMailboxTab
    {
        PersistedTabCommon common;
        std::string mailboxId;
        std::optional<std::string> mailboxRole;
        std::size_t offset = 0;
    };

    struct PersistedSearchTab
    {
        PersistedTabCommon common;
        javelin::gui::search::PersistedSearchState search;
    };

    struct PersistedComposeTab
    {
        PersistedTabCommon common;
        std::string composeSessionId;
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

    [[nodiscard]] PersistedMainWindowState
    readMainWindowState(QSettings& settings, javelin::jmap::query::EmailListSort defaultSort);
    void writeMainWindowState(QSettings& settings, const PersistedMainWindowState& state);

    [[nodiscard]] PersistedMainWindowState
    loadMainWindowState(javelin::jmap::query::EmailListSort defaultSort);
    void saveMainWindowState(const PersistedMainWindowState& state);
    void saveEmailListSort(javelin::jmap::query::EmailListSort sort);
} // namespace javelin::gui::shell
