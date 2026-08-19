#include "gui/shell/MessageListPresentationPolicy.h"

#include <utility>

namespace javelin::gui::shell
{
    MessageListPresentationPlan
    planMessageListPresentation(const MessageListPresentationInput& input)
    {
        using javelin::gui::messages::MessageListEmptyAction;
        using javelin::gui::messages::MessageListEmptyState;
        using javelin::gui::messages::MessageListEmptyStateKind;

        MessageListEmptyState emptyState{
            .itemCount = input.itemCount,
            .kind = MessageListEmptyStateKind::EmptyMailbox,
            .action = MessageListEmptyAction::None,
            .detail = {},
        };
        if (input.accountStatus == javelin::app::MailAccountStatus::AuthenticationPaused &&
            (!input.cacheLoaded || !input.refreshError.isEmpty()))
        {
            emptyState.kind = MessageListEmptyStateKind::AuthenticationRequired;
            emptyState.action = MessageListEmptyAction::SignInAgain;
        }
        else if (input.accountStatus == javelin::app::MailAccountStatus::Disconnected &&
                 (!input.cacheLoaded || !input.refreshError.isEmpty()))
        {
            emptyState.kind = MessageListEmptyStateKind::Disconnected;
            emptyState.action = MessageListEmptyAction::Retry;
        }
        else if (input.accountStatus == javelin::app::MailAccountStatus::Connecting &&
                 !input.cacheLoaded)
        {
            emptyState.kind = MessageListEmptyStateKind::Connecting;
        }
        else if (!input.refreshError.isEmpty())
        {
            emptyState.kind = MessageListEmptyStateKind::RefreshFailed;
            emptyState.action = MessageListEmptyAction::Retry;
            emptyState.detail = input.refreshError;
        }
        else if (input.refreshInFlight && input.itemCount == 0)
        {
            emptyState.kind = MessageListEmptyStateKind::Loading;
        }
        else if (!input.cacheLoaded)
        {
            emptyState.kind = MessageListEmptyStateKind::NotYetLoaded;
            emptyState.action = MessageListEmptyAction::Retry;
        }
        else if (input.tabKind == TabKind::Mailbox && input.quickFilterActive)
        {
            emptyState.kind = MessageListEmptyStateKind::NoFilterMatches;
            emptyState.action = MessageListEmptyAction::ClearFilters;
        }
        else if (input.tabKind == TabKind::Search && input.localSearch)
        {
            emptyState.kind = MessageListEmptyStateKind::NoLocalSearchResults;
            emptyState.action = input.canSearchServer ? MessageListEmptyAction::SearchServer
                                                      : MessageListEmptyAction::EditSearch;
        }
        else if (input.tabKind == TabKind::Search)
        {
            emptyState.kind = MessageListEmptyStateKind::NoSearchResults;
            emptyState.action = MessageListEmptyAction::EditSearch;
        }
        else
        {
            emptyState.kind = MessageListEmptyStateKind::EmptyMailbox;
        }

        MessageListHeaderPresentation header;
        if (!input.tabKind.has_value())
        {
            header = std::monostate{};
        }
        else if (input.list.has_value())
        {
            auto list = *input.list;
            list.refreshInFlight = input.refreshInFlight;
            header = std::move(list);
        }
        else
        {
            QString context;
            switch (*input.tabKind)
            {
            case TabKind::Compose:
                context = QStringLiteral("Compose");
                break;
            case TabKind::Contacts:
                context = QStringLiteral("Contacts");
                break;
            case TabKind::Calendar:
                context = QStringLiteral("Calendar");
                break;
            case TabKind::Mailbox:
            case TabKind::Search:
                break;
            }
            header = javelin::gui::messages::MessageListContextHeader{
                .title = input.title,
                .context = std::move(context),
            };
        }

        return {
            .emptyState = std::move(emptyState),
            .header = std::move(header),
        };
    }
} // namespace javelin::gui::shell
