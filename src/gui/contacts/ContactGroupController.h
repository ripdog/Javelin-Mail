#pragma once

#include "app/ContactApplicationPorts.h"

#include <QString>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class QObject;

namespace javelin::gui::contacts
{
    class ContactGroupController final
    {
      public:
        ContactGroupController(
            javelin::app::ContactCommandPort& commandPort, QObject& context,
            std::function<std::optional<std::string>(std::string_view)> ownerAccountId,
            std::function<void(bool)> setBusy, std::function<void(QString, int)> statusMessage);

        void setMembership(std::string accountId, std::string groupId,
                           std::vector<std::string> memberUids, bool included);
        void deleteGroup(std::string accountId, std::string groupId);

      private:
        javelin::app::ContactCommandPort& m_commandPort;
        QObject& m_context;
        std::function<std::optional<std::string>(std::string_view)> m_ownerAccountId;
        std::function<void(bool)> m_setBusy;
        std::function<void(QString, int)> m_statusMessage;
    };
} // namespace javelin::gui::contacts
