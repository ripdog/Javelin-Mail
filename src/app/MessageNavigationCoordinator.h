#pragma once

#include <QObject>

#include <cstdint>
#include <optional>
#include <string>

namespace javelin::app
{

    struct OpenEmailRoute
    {
        std::uint64_t id = 0;
        std::string accountId;
        std::string mailboxId;
        std::optional<std::string> threadId;
        std::string emailId;
    };

    class MessageNavigationCoordinator final : public QObject
    {
        Q_OBJECT

      public:
        explicit MessageNavigationCoordinator(QObject* parent = nullptr);

        [[nodiscard]] std::uint64_t openEmail(std::string accountId, std::string mailboxId,
                                              std::optional<std::string> threadId,
                                              std::string emailId);
        [[nodiscard]] const std::optional<OpenEmailRoute>& currentRoute() const;
        [[nodiscard]] bool isCurrent(std::uint64_t routeId) const;
        void complete(std::uint64_t routeId);
        void cancel();

      Q_SIGNALS:
        void routeRequested(const javelin::app::OpenEmailRoute& route);
        void routeCleared(std::uint64_t routeId);

      private:
        std::optional<OpenEmailRoute> m_currentRoute;
        std::uint64_t m_nextRouteId = 1;
    };

} // namespace javelin::app
