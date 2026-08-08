#include "gui/shell/TabPersistence.h"

#include "gui/calendar/MonthCalendarWidget.h"
#include "gui/compose/ComposeTabWidget.h"
#include "gui/contacts/ContactsManagerWidget.h"

#include <type_traits>

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

    PersistedTab persistTab(const TabState& tab)
    {
        return std::visit(
            [](const auto& content) -> PersistedTab
            {
                using Content = std::decay_t<decltype(content)>;
                if constexpr (std::is_same_v<Content, MailboxTabState>)
                {
                    return persistMailboxTab({
                        .accountId = content.session->accountId(),
                        .title = content.session->title(),
                        .selection = content.selection,
                        .mailboxId = content.session->mailboxId(),
                        .mailboxRole = content.session->role(),
                        .windows = content.session->windowRequests(),
                    });
                }
                else if constexpr (std::is_same_v<Content, SearchTabState>)
                {
                    return persistSearchTab({
                        .accountId = content.session->accountId(),
                        .title = content.session->title(),
                        .selection = content.selection,
                        .criteria = content.session->criteria(),
                        .mode = content.session->mode(),
                        .sessionId = content.session->sessionId(),
                        .windows = content.session->windowRequests(),
                    });
                }
                else if constexpr (std::is_same_v<Content, ComposeTabState>)
                {
                    return PersistedComposeTab{
                        .common =
                            {
                                .title = content.title,
                                .selection = persistSelection(content.selection),
                            },
                        .accountId = content.accountId,
                        .composeSessionId = content.composeSessionId,
                        .hasUnsavedChanges =
                            content.widget != nullptr && content.widget->hasUnsavedChanges(),
                    };
                }
                else if constexpr (std::is_same_v<Content, ContactsTabState>)
                {
                    return persistContactsTab(content,
                                              content.widget != nullptr
                                                  ? content.widget->viewState()
                                                  : javelin::gui::contacts::ContactsViewState{});
                }
                else
                {
                    return persistCalendarTab(content, content.widget != nullptr
                                                           ? content.widget->displayedMonth()
                                                           : QDate{});
                }
            },
            tab.content);
    }
} // namespace javelin::gui::shell
