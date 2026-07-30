#pragma once

#include "app/MessageNavigationCoordinator.h"
#include "gui/messages/MessageSelectionRestoration.h"
#include "gui/shell/TabWorkspace.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace javelin::gui::shell
{
    class MessageListTabController;

    struct MessageNavigationResolution
    {
        std::optional<javelin::app::OpenEmailRoute> route;
        std::optional<std::size_t> currentRow;
        bool completeRoute = false;
    };

    class MessageNavigationController
    {
      public:
        MessageNavigationController(javelin::app::MessageNavigationCoordinator& coordinator,
                                    MessageListTabController& messageListTabController);

        void begin(TabState& tab, const javelin::app::OpenEmailRoute& route);
        [[nodiscard]] const javelin::app::OpenEmailRoute*
        activeRoute(const TabState* activeTab) const;
        [[nodiscard]] MessageNavigationResolution
        resolve(TabState* activeTab,
                std::span<const javelin::gui::messages::MessageRowIdentity> rows);
        void complete(std::uint64_t routeId);
        void cancelIfSelectionChanged(const TabState* activeTab, std::string_view selectedEmailId,
                                      std::optional<std::string_view> selectedThreadId);

      private:
        javelin::app::MessageNavigationCoordinator& m_coordinator;
        MessageListTabController& m_messageListTabController;
        std::optional<std::uint64_t> m_startedRouteId;
        std::optional<std::uint64_t> m_revealRequestedForRoute;
    };
} // namespace javelin::gui::shell
