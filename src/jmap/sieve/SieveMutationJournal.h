#pragma once

#include "jmap/cache/SieveRepository.h"
#include "jmap/sync/MutationJournal.h"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::sieve
{
    enum class SieveMutationKind
    {
        Create,
        Update,
        Destroy,
        Activate,
    };

    struct SieveMutationRecord
    {
        std::string mutationId;
        std::optional<std::string> operationGroupId;
        std::string accountId;
        std::string objectId;
        SieveMutationKind kind = SieveMutationKind::Update;
        sync::MutationStatus status = sync::MutationStatus::Pending;
        std::vector<SieveScript> baseScripts;
        std::vector<SieveScript> projectedScripts;
        std::optional<std::string> baseState;
        std::optional<std::string> acceptedState;
        std::optional<std::string> errorJson;
    };

    class SieveMutationJournal
    {
      public:
        SieveMutationJournal(cache::DatabaseConnection& connection,
                             cache::SieveRepository& repository);

        [[nodiscard]] std::optional<cache::DatabaseError> queue(const SieveMutationRecord& record);
        [[nodiscard]] std::optional<cache::DatabaseError>
        transition(const SieveMutationRecord& record, sync::MutationStatus status,
                   std::optional<std::string_view> acceptedState = std::nullopt,
                   std::optional<std::string_view> errorJson = std::nullopt);
        [[nodiscard]] std::optional<cache::DatabaseError>
        restoreRejected(const SieveMutationRecord& record,
                        std::optional<std::string_view> acceptedState = std::nullopt,
                        std::optional<std::string_view> errorJson = std::nullopt);

      private:
        cache::DatabaseConnection& m_connection;
        cache::SieveRepository& m_repository;
    };
} // namespace javelin::jmap::sieve
