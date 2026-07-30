#include "gui/shell/MessageNavigationController.h"

#include "gui/shell/MessageListTabController.h"
#include "gui/shell/MessageNavigationPolicy.h"

namespace javelin::gui::shell
{
    MessageNavigationController::MessageNavigationController(
        javelin::app::MessageNavigationCoordinator& coordinator,
        MessageListTabController& messageListTabController)
        : m_coordinator(coordinator), m_messageListTabController(messageListTabController)
    {
    }

    void MessageNavigationController::begin(TabState& tab,
                                            const javelin::app::OpenEmailRoute& route)
    {
        tabSelection(tab) = {
            .threadId = route.threadId,
            .emailId = route.emailId,
            .selectedEmailIds = {},
        };
        m_startedRouteId = route.id;
        m_revealRequestedForRoute.reset();
    }

    const javelin::app::OpenEmailRoute*
    MessageNavigationController::activeRoute(const TabState* activeTab) const
    {
        const auto& route = m_coordinator.currentRoute();
        if (!route.has_value() || activeTab == nullptr ||
            !isStartedMessageNavigationRoute(
                &*route, m_startedRouteId,
                tabAccountId(*activeTab)
                    .transform([](const auto& id) { return std::string_view{id}; }),
                tabMailboxId(*activeTab)
                    .transform([](const auto& id) { return std::string_view{id}; })))
            return nullptr;
        return &*route;
    }

    MessageNavigationResolution MessageNavigationController::resolve(
        TabState* activeTab, const std::span<const javelin::gui::messages::MessageRowIdentity> rows)
    {
        const auto* route = activeRoute(activeTab);
        if (route == nullptr)
            return {};

        const auto routeCopy = *route;
        const auto plan = planMessageNavigation({
            .route = route,
            .activeAccountId = tabAccountId(*activeTab),
            .activeMailboxId = tabMailboxId(*activeTab),
            .rows = rows,
            .mailboxRefreshInFlight = m_messageListTabController.pageRefreshInFlight(*activeTab),
            .revealAlreadyRequested =
                m_revealRequestedForRoute == std::optional<std::uint64_t>{route->id},
        });
        if (!plan.presentRoute)
            return {};

        if (plan.requestReveal && m_messageListTabController.reveal(*activeTab, route->emailId))
            m_revealRequestedForRoute = route->id;

        return {
            .route = routeCopy,
            .currentRow = plan.currentRow,
            .completeRoute = plan.completeRoute,
        };
    }

    void MessageNavigationController::complete(const std::uint64_t routeId)
    {
        if (m_startedRouteId == std::optional<std::uint64_t>{routeId})
            m_startedRouteId.reset();
        m_revealRequestedForRoute.reset();
        m_coordinator.complete(routeId);
    }

    void MessageNavigationController::cancelIfSelectionChanged(
        const TabState* activeTab, const std::string_view selectedEmailId,
        const std::optional<std::string_view> selectedThreadId)
    {
        const auto* route = activeRoute(activeTab);
        if (route == nullptr ||
            !shouldCancelMessageNavigation(*route, selectedEmailId, selectedThreadId))
        {
            return;
        }

        m_startedRouteId.reset();
        m_revealRequestedForRoute.reset();
        m_coordinator.cancel();
    }
} // namespace javelin::gui::shell
