#pragma once

#include "app/ContactApplicationPorts.h"
#include "jmap/api/ContactsMethods.h"
#include "jmap/cache/ContactReader.h"

#include <QString>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class QObject;

namespace javelin::gui::contacts
{
    class AddressBookController final
    {
      public:
        AddressBookController(
            javelin::app::ContactCommandPort& commandPort,
            javelin::jmap::cache::ContactReader& repository, QObject& context,
            std::function<std::optional<std::string>(std::string_view)> ownerAccountId,
            std::function<void(bool)> setBusy, std::function<void(QString, int)> statusMessage);

        [[nodiscard]] bool
        canSetSubscription(const std::vector<javelin::jmap::cache::ContactAccount>& accounts,
                           std::string_view accountId, const javelin::jmap::api::AddressBook& book,
                           bool subscribed) const;
        void mutate(javelin::app::AddressBookCommand command, QString progressMessage);

      private:
        javelin::app::ContactCommandPort& m_commandPort;
        javelin::jmap::cache::ContactReader& m_repository;
        QObject& m_context;
        std::function<std::optional<std::string>(std::string_view)> m_ownerAccountId;
        std::function<void(bool)> m_setBusy;
        std::function<void(QString, int)> m_statusMessage;
    };
} // namespace javelin::gui::contacts
