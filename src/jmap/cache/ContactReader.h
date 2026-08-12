#pragma once

#include "jmap/api/ContactsMethods.h"
#include "jmap/contacts/ContactTypes.h"
#include "storage/DatabaseError.h"

#include <QObject>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    struct ContactAccount
    {
        std::string accountId;
        std::string ownerAccountId;
        std::string name;
        bool isReadOnly = false;
        bool mayCreateAddressBook = false;
    };

    class ContactReader
    {
      public:
        virtual ~ContactReader() = default;

        [[nodiscard]] virtual QMetaObject::Connection
        connectChanged(QObject* context, std::function<void(const QString&)> callback) = 0;

        [[nodiscard]] virtual std::variant<std::vector<javelin::jmap::api::AddressBook>,
                                           DatabaseError>
        listAddressBooks(std::string_view accountId, bool includeUnsubscribed = true) const = 0;
        [[nodiscard]] virtual std::variant<std::optional<std::string>, DatabaseError>
        addressBookState(std::string_view accountId) const = 0;
        [[nodiscard]] virtual std::variant<std::optional<std::string>, DatabaseError>
        contactState(std::string_view accountId) const = 0;
        [[nodiscard]] virtual std::variant<std::vector<ContactAccount>, DatabaseError>
        listAccounts(std::optional<std::string_view> ownerAccountId = std::nullopt) const = 0;
        [[nodiscard]] virtual std::variant<std::vector<javelin::jmap::contacts::ContactSummary>,
                                           DatabaseError>
        listContacts(std::string_view accountId,
                     std::optional<std::string_view> addressBookId = std::nullopt,
                     std::string_view filter = {}) const = 0;
        [[nodiscard]] virtual std::variant<std::optional<javelin::jmap::contacts::ContactSummary>,
                                           DatabaseError>
        findContact(std::string_view accountId, std::string_view contactId) const = 0;
        [[nodiscard]] virtual std::variant<std::optional<javelin::jmap::contacts::ContactSummary>,
                                           DatabaseError>
        findByEmail(std::string_view normalizedEmail,
                    std::optional<std::string_view> accountId = std::nullopt) const = 0;
        [[nodiscard]] virtual std::variant<std::vector<std::string>, DatabaseError>
        listEmailAddresses() const = 0;
    };

} // namespace javelin::jmap::cache
