#pragma once

#include "app/ContactApplicationPorts.h"
#include "jmap/contacts/ContactTypes.h"

#include <QByteArray>
#include <QString>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

class QObject;

namespace javelin::gui::contacts
{
    class ContactPhotoController final
    {
      public:
        ContactPhotoController(
            javelin::app::ContactCommandPort& commandPort, QObject& context,
            std::function<std::optional<std::string>(std::string_view)> ownerAccountId,
            std::function<void(bool)> setBusy, std::function<void(QString, int)> statusMessage);

        void upload(const std::string& accountId, QByteArray payload, std::string mediaType,
                    std::function<std::string()> currentDocument,
                    std::function<void(std::string)> setDocument,
                    std::function<void(const QByteArray&)> showPhoto,
                    std::function<void(bool)> setRemoveEnabled);
        [[nodiscard]] std::variant<std::string, QString>
        remove(const std::string& document, std::function<void()> clearPhoto,
               std::function<void(bool)> setRemoveEnabled) const;
        void show(const javelin::jmap::contacts::ContactSummary& contact,
                  std::function<void()> clearPhoto,
                  std::function<void(const QByteArray&)> showPhoto,
                  std::function<bool(const std::string&)> stillSelected);

      private:
        javelin::app::ContactCommandPort& m_commandPort;
        QObject& m_context;
        std::function<std::optional<std::string>(std::string_view)> m_ownerAccountId;
        std::function<void(bool)> m_setBusy;
        std::function<void(QString, int)> m_statusMessage;
    };
} // namespace javelin::gui::contacts
