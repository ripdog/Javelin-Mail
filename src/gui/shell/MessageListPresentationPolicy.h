#pragma once

#include "gui/messages/MessageListPanePresenter.h"
#include "gui/shell/TabWorkspace.h"

#include <QString>

#include <cstddef>
#include <optional>
#include <variant>

namespace javelin::gui::shell
{
    struct MessageListPresentationInput
    {
        std::optional<TabKind> tabKind;
        QString title;
        std::size_t itemCount = 0;
        QString refreshError;
        bool refreshInFlight = false;
        bool localSearch = false;
        std::optional<javelin::gui::messages::MessageListPageHeader> page;
    };

    using MessageListHeaderPresentation =
        std::variant<std::monostate, javelin::gui::messages::MessageListContextHeader,
                     javelin::gui::messages::MessageListPageHeader>;

    struct MessageListPresentationPlan
    {
        javelin::gui::messages::MessageListEmptyState emptyState;
        MessageListHeaderPresentation header;
    };

    [[nodiscard]] MessageListPresentationPlan
    planMessageListPresentation(const MessageListPresentationInput& input);
} // namespace javelin::gui::shell
