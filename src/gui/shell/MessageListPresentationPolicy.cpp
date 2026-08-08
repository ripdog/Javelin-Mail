#include "gui/shell/MessageListPresentationPolicy.h"

#include <utility>

namespace javelin::gui::shell
{
    MessageListPresentationPlan
    planMessageListPresentation(const MessageListPresentationInput& input)
    {
        auto collection = javelin::gui::messages::MessageCollectionKind::Mailbox;
        if (input.tabKind == TabKind::Search)
        {
            collection = input.localSearch
                             ? javelin::gui::messages::MessageCollectionKind::LocalSearch
                             : javelin::gui::messages::MessageCollectionKind::OnlineSearch;
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
            .emptyState =
                {
                    .itemCount = input.itemCount,
                    .refreshError = input.refreshError,
                    .refreshInFlight = input.refreshInFlight,
                    .collection = collection,
                },
            .header = std::move(header),
        };
    }
} // namespace javelin::gui::shell
