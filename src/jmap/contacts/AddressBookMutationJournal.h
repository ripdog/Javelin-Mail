#pragma once

#include "jmap/cache/ContactRepository.h"
#include "jmap/sync/MutationJournal.h"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::contacts
{
    enum class AddressBookMutationKind
    {
        Create,
        Update,
        Destroy,
    };

    struct AddressBookMutationRecord
    {
        std::string mutationId;
        std::optional<std::string> operationGroupId;
        std::string accountId;
        std::string objectId;
        std::optional<std::string> creationId;
        AddressBookMutationKind kind = AddressBookMutationKind::Update;
        sync::MutationStatus status = sync::MutationStatus::Pending;
        std::string requestedDocument;
        std::optional<std::string> baseDocument;
        std::optional<std::string> projectedDocument;
        std::optional<std::string> baseState;
        std::optional<std::string> acceptedState;
        std::optional<std::string> errorJson;
    };

    class AddressBookMutationJournal
    {
      public:
        AddressBookMutationJournal(cache::DatabaseConnection& connection,
                                   cache::ContactRepository& contacts);

        [[nodiscard]] std::optional<cache::DatabaseError>
        queue(const std::vector<AddressBookMutationRecord>& records,
              const std::vector<api::AddressBook>& projectedBooks, std::string_view state);
        [[nodiscard]] std::variant<std::vector<AddressBookMutationRecord>, cache::DatabaseError>
        listForAddressBook(std::string_view accountId, std::string_view addressBookId) const;
        [[nodiscard]] std::variant<std::vector<AddressBookMutationRecord>, cache::DatabaseError>
        listActive(std::string_view accountId) const;
        [[nodiscard]] std::optional<cache::DatabaseError>
        transition(const std::vector<AddressBookMutationRecord>& records,
                   sync::MutationStatus status,
                   std::optional<std::string_view> acceptedState = std::nullopt,
                   std::optional<std::string_view> errorJson = std::nullopt);

      private:
        cache::DatabaseConnection& m_connection;
        cache::ContactRepository& m_contacts;
        sync::MutationJournalRepository m_journal;
    };
} // namespace javelin::jmap::contacts
