#pragma once

#include "storage/sqlite/DatabaseConnection.h"

#include "jmap/cache/ContactRepository.h"
#include "jmap/sync/MutationJournal.h"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::contacts
{
    enum class ContactMutationKind
    {
        Create,
        Update,
        Destroy,
    };

    struct ContactMutationRecord
    {
        std::string mutationId;
        std::optional<std::string> operationGroupId;
        std::string accountId;
        std::string objectId;
        std::optional<std::string> creationId;
        ContactMutationKind kind = ContactMutationKind::Update;
        sync::MutationStatus status = sync::MutationStatus::Pending;
        std::string requestedDocument;
        std::optional<std::string> baseDocument;
        std::optional<std::string> projectedDocument;
        std::optional<std::string> baseState;
        std::optional<std::string> acceptedState;
        std::optional<std::string> errorJson;
    };

    struct ContactProjection
    {
        std::string accountId;
        std::vector<ContactSummary> contacts;
        std::vector<std::string> destroyedIds;
    };

    class ContactMutationJournal
    {
      public:
        ContactMutationJournal(cache::DatabaseConnection& connection,
                               cache::ContactRepository& contacts);

        [[nodiscard]] std::optional<cache::DatabaseError>
        queue(const std::vector<ContactMutationRecord>& records,
              const std::vector<ContactSummary>& projectedContacts,
              std::span<const std::string> destroyedIds);
        [[nodiscard]] std::optional<cache::DatabaseError>
        queueGroup(const std::vector<ContactMutationRecord>& records,
                   std::span<const ContactProjection> projections);
        [[nodiscard]] std::variant<std::vector<ContactMutationRecord>, cache::DatabaseError>
        listForContact(std::string_view accountId, std::string_view contactId) const;
        [[nodiscard]] std::variant<std::vector<ContactMutationRecord>, cache::DatabaseError>
        listActive(std::string_view accountId) const;
        [[nodiscard]] std::optional<cache::DatabaseError>
        transition(const std::vector<ContactMutationRecord>& records, sync::MutationStatus status,
                   std::optional<std::string_view> acceptedState = std::nullopt,
                   std::optional<std::string_view> errorJson = std::nullopt);
        [[nodiscard]] std::optional<cache::DatabaseError>
        restoreRejected(const std::vector<ContactMutationRecord>& records,
                        std::optional<std::string_view> errorJson = std::nullopt);

      private:
        cache::DatabaseConnection& m_connection;
        cache::ContactRepository& m_contacts;
        sync::MutationJournalRepository m_journal;
    };
} // namespace javelin::jmap::contacts
