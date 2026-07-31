#pragma once

#include "jmap/api/ContactsMethods.h"
#include "jmap/cache/ContactReader.h"

#include <QObject>

#include <optional>
#include <string_view>
#include <variant>

namespace javelin::jmap::cache
{
    class ContactRepository final : public QObject, public ContactReader
    {
        Q_OBJECT

      public:
        explicit ContactRepository(DatabaseConnection& connection);
        explicit ContactRepository(ReadOnlyDatabaseConnection& connection);

        [[nodiscard]] QMetaObject::Connection
        connectChanged(QObject* context, std::function<void(const QString&)> callback) override;

        [[nodiscard]] std::optional<DatabaseError>
        replaceAll(std::string_view accountId,
                   const std::vector<javelin::jmap::api::AddressBook>& books,
                   const std::vector<javelin::jmap::contacts::ContactSummary>& contacts,
                   std::string_view addressBookState, std::string_view contactState);
        [[nodiscard]] std::optional<DatabaseError>
        replaceAll(DatabaseTransaction& transaction, std::string_view accountId,
                   const std::vector<javelin::jmap::api::AddressBook>& books,
                   const std::vector<javelin::jmap::contacts::ContactSummary>& contacts,
                   std::string_view addressBookState, std::string_view contactState);
        [[nodiscard]] std::optional<DatabaseError>
        replaceAddressBooks(std::string_view accountId,
                            const std::vector<javelin::jmap::api::AddressBook>& books,
                            std::string_view state);
        [[nodiscard]] std::optional<DatabaseError>
        replaceAddressBooks(DatabaseTransaction& transaction, std::string_view accountId,
                            const std::vector<javelin::jmap::api::AddressBook>& books,
                            std::string_view state);
        [[nodiscard]] std::optional<DatabaseError>
        upsertContacts(std::string_view accountId,
                       const std::vector<javelin::jmap::contacts::ContactSummary>& contacts,
                       std::span<const std::string> destroyed, std::string_view state);
        [[nodiscard]] std::optional<DatabaseError>
        upsertContacts(DatabaseTransaction& transaction, std::string_view accountId,
                       const std::vector<javelin::jmap::contacts::ContactSummary>& contacts,
                       std::span<const std::string> destroyed, std::string_view state);
        [[nodiscard]] std::optional<DatabaseError>
        projectContacts(DatabaseTransaction& transaction, std::string_view accountId,
                        const std::vector<javelin::jmap::contacts::ContactSummary>& contacts,
                        std::span<const std::string> destroyed);
        [[nodiscard]] std::variant<std::vector<javelin::jmap::api::AddressBook>, DatabaseError>
        listAddressBooks(std::string_view accountId,
                         bool includeUnsubscribed = true) const override;
        [[nodiscard]] std::variant<std::optional<std::string>, DatabaseError>
        addressBookState(std::string_view accountId) const override;
        [[nodiscard]] std::variant<std::optional<std::string>, DatabaseError>
        contactState(std::string_view accountId) const override;
        [[nodiscard]] std::variant<std::vector<ContactAccount>, DatabaseError>
        listAccounts(std::optional<std::string_view> ownerAccountId = std::nullopt) const override;
        [[nodiscard]] std::variant<std::vector<javelin::jmap::contacts::ContactSummary>,
                                   DatabaseError>
        listContacts(std::string_view accountId,
                     std::optional<std::string_view> addressBookId = std::nullopt,
                     std::string_view filter = {}) const override;
        [[nodiscard]] std::variant<std::optional<javelin::jmap::contacts::ContactSummary>,
                                   DatabaseError>
        findContact(std::string_view accountId, std::string_view contactId) const override;
        [[nodiscard]] std::variant<std::optional<javelin::jmap::contacts::ContactSummary>,
                                   DatabaseError>
        findByEmail(std::string_view normalizedEmail,
                    std::optional<std::string_view> accountId = std::nullopt) const override;
        void notifyChanged(std::string_view accountId);

      Q_SIGNALS:
        void contactsChanged(const QString& accountId);

      private:
        DatabaseReadView m_connection;
        DatabaseConnection* m_writeConnection = nullptr;
    };
} // namespace javelin::jmap::cache
