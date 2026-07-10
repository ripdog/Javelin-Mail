#pragma once

#include "jmap/api/ContactsMethods.h"
#include "jmap/cache/Database.h"
#include "jmap/contacts/ContactTypes.h"

#include <optional>
#include <string_view>
#include <variant>

namespace javelin::jmap::cache
{
    class ContactRepository
    {
      public:
        explicit ContactRepository(DatabaseConnection& connection);

        [[nodiscard]] std::optional<DatabaseError>
        replaceAll(std::string_view accountId,
                   const std::vector<javelin::jmap::api::AddressBook>& books,
                   const std::vector<javelin::jmap::contacts::ContactSummary>& contacts,
                   std::string_view addressBookState, std::string_view contactState);
        [[nodiscard]] std::optional<DatabaseError>
        upsertContacts(std::string_view accountId,
                       const std::vector<javelin::jmap::contacts::ContactSummary>& contacts,
                       std::span<const std::string> destroyed, std::string_view state);
        [[nodiscard]] std::variant<std::vector<javelin::jmap::api::AddressBook>, DatabaseError>
        listAddressBooks(std::string_view accountId, bool includeUnsubscribed = true) const;
        [[nodiscard]] std::variant<std::vector<javelin::jmap::contacts::ContactSummary>,
                                   DatabaseError>
        listContacts(std::string_view accountId,
                     std::optional<std::string_view> addressBookId = std::nullopt,
                     std::string_view filter = {}) const;
        [[nodiscard]] std::variant<std::optional<javelin::jmap::contacts::ContactSummary>,
                                   DatabaseError>
        findByEmail(std::string_view normalizedEmail,
                    std::optional<std::string_view> accountId = std::nullopt) const;

      private:
        DatabaseConnection& m_connection;
    };
} // namespace javelin::jmap::cache
