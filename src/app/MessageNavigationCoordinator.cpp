#include "app/MessageNavigationCoordinator.h"

#include <utility>

namespace javelin::app
{

    MessageNavigationCoordinator::MessageNavigationCoordinator(QObject* parent) : QObject(parent)
    {
    }

    std::uint64_t MessageNavigationCoordinator::openEmail(std::string accountId,
                                                          std::string mailboxId,
                                                          std::optional<std::string> threadId,
                                                          std::string emailId)
    {
        const auto routeId = m_nextRouteId++;
        m_currentRoute = OpenEmailRoute{
            .id = routeId,
            .accountId = std::move(accountId),
            .mailboxId = std::move(mailboxId),
            .threadId = std::move(threadId),
            .emailId = std::move(emailId),
        };
        Q_EMIT routeRequested(*m_currentRoute);
        return routeId;
    }

    const std::optional<OpenEmailRoute>& MessageNavigationCoordinator::currentRoute() const
    {
        return m_currentRoute;
    }

    bool MessageNavigationCoordinator::isCurrent(const std::uint64_t routeId) const
    {
        return m_currentRoute.has_value() && m_currentRoute->id == routeId;
    }

    void MessageNavigationCoordinator::complete(const std::uint64_t routeId)
    {
        if (!isCurrent(routeId))
        {
            return;
        }
        m_currentRoute.reset();
        Q_EMIT routeCleared(routeId);
    }

    void MessageNavigationCoordinator::cancel()
    {
        if (!m_currentRoute.has_value())
        {
            return;
        }
        const auto routeId = m_currentRoute->id;
        m_currentRoute.reset();
        Q_EMIT routeCleared(routeId);
    }

} // namespace javelin::app
