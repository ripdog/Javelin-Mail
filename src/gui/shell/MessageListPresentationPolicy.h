#pragma once

#include "app/MailApplicationEventsPorts.h"
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
        bool cacheLoaded = false;
        bool quickFilterActive = false;
        bool localSearch = false;
        bool canSearchServer = false;
        std::optional<javelin::app::MailAccountStatus> accountStatus;
        std::optional<javelin::gui::messages::MessageListHeader> list;
    };

    using MessageListHeaderPresentation =
        std::variant<std::monostate, javelin::gui::messages::MessageListContextHeader,
                     javelin::gui::messages::MessageListHeader>;

    struct MessageListPresentationPlan
    {
        javelin::gui::messages::MessageListEmptyState emptyState;
        MessageListHeaderPresentation header;
    };

    [[nodiscard]] MessageListPresentationPlan
    planMessageListPresentation(const MessageListPresentationInput& input);
} // namespace javelin::gui::shell
