#pragma once

#include "gui/shell/MessageListPresentationPolicy.h"
#include "gui/shell/TabWorkspace.h"

#include <cstddef>

namespace javelin::gui::messages
{
    class MessageListPanePresenter;
}

namespace javelin::gui::shell
{
    class TabBarPresenter;

    class MessageListTabPresenter
    {
      public:
        MessageListTabPresenter(javelin::gui::messages::MessageListPanePresenter& panePresenter,
                                const TabBarPresenter& tabBarPresenter);

        [[nodiscard]] javelin::gui::messages::MessageListEmptyAction
        showEmptyState(const TabState* tab, std::size_t itemCount,
                       std::optional<javelin::app::MailAccountStatus> accountStatus) const;
        void showHeader(const TabState* tab) const;

      private:
        [[nodiscard]] MessageListPresentationInput
        inputFor(const TabState* tab, std::size_t itemCount,
                 std::optional<javelin::app::MailAccountStatus> accountStatus = std::nullopt) const;

        javelin::gui::messages::MessageListPanePresenter& m_panePresenter;
        const TabBarPresenter& m_tabBarPresenter;
    };
} // namespace javelin::gui::shell
