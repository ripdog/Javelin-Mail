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
        std::optional<std::string> mailboxName;
        std::optional<std::string> threadId;
        std::string emailId;
    };

    class MessageNavigationPort : public QObject
    {
        Q_OBJECT

      public:
        using QObject::QObject;
        ~MessageNavigationPort() override = default;

        [[nodiscard]] virtual std::uint64_t
        openEmail(std::string accountId, std::string mailboxId, std::optional<std::string> threadId,
                  std::string emailId, std::optional<std::string> mailboxName = std::nullopt) = 0;
        [[nodiscard]] virtual const std::optional<OpenEmailRoute>& currentRoute() const = 0;
        [[nodiscard]] virtual bool isCurrent(std::uint64_t routeId) const = 0;
        virtual void complete(std::uint64_t routeId) = 0;
        virtual void cancel() = 0;

      Q_SIGNALS:
        void routeRequested(const javelin::app::OpenEmailRoute& route);
        void routeCleared(std::uint64_t routeId);
    };
} // namespace javelin::app
