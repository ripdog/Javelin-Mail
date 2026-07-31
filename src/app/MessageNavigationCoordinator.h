#pragma once

#include "app/MessageNavigationPort.h"

namespace javelin::app
{

    class MessageNavigationCoordinator final : public MessageNavigationPort
    {
        Q_OBJECT

      public:
        explicit MessageNavigationCoordinator(QObject* parent = nullptr);

        [[nodiscard]] std::uint64_t openEmail(std::string accountId, std::string mailboxId,
                                              std::optional<std::string> threadId,
                                              std::string emailId) override;
        [[nodiscard]] const std::optional<OpenEmailRoute>& currentRoute() const override;
        [[nodiscard]] bool isCurrent(std::uint64_t routeId) const override;
        void complete(std::uint64_t routeId) override;
        void cancel() override;

      private:
        std::optional<OpenEmailRoute> m_currentRoute;
        std::uint64_t m_nextRouteId = 1;
    };

} // namespace javelin::app
