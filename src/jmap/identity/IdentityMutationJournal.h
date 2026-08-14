#pragma once

#include "storage/sqlite/DatabaseConnection.h"

#include "jmap/cache/IdentityRepository.h"
#include "jmap/sync/MutationJournal.h"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::identity
{
    enum class IdentityMutationKind
    {
        Create,
        Update,
        Destroy,
    };

    struct IdentityMutationRecord
    {
        std::string mutationId;
        std::optional<std::string> operationGroupId;
        std::string accountId;
        std::string objectId;
        std::optional<std::string> creationId;
        IdentityMutationKind kind = IdentityMutationKind::Update;
        sync::MutationStatus status = sync::MutationStatus::Pending;
        std::optional<javelin::jmap::domain::Identity> before;
        std::optional<javelin::jmap::domain::Identity> after;
        std::optional<std::string> baseState;
        std::optional<std::string> acceptedState;
        std::optional<std::string> errorJson;
    };

    class IdentityMutationJournal
    {
      public:
        IdentityMutationJournal(cache::DatabaseConnection& connection,
                                cache::IdentityRepository& repository);

        [[nodiscard]] std::optional<cache::DatabaseError>
        queue(const IdentityMutationRecord& record);
        [[nodiscard]] std::optional<cache::DatabaseError>
        transition(const IdentityMutationRecord& record, sync::MutationStatus status,
                   std::optional<std::string_view> acceptedState = std::nullopt,
                   std::optional<std::string_view> errorJson = std::nullopt);
        [[nodiscard]] std::optional<cache::DatabaseError>
        reject(const IdentityMutationRecord& record,
               std::optional<std::string_view> acceptedState = std::nullopt,
               std::optional<std::string_view> errorJson = std::nullopt);
        [[nodiscard]] std::optional<cache::DatabaseError>
        accept(const IdentityMutationRecord& record,
               const std::vector<javelin::jmap::domain::Identity>& confirmed,
               std::string_view state);
        [[nodiscard]] std::variant<std::vector<IdentityMutationRecord>, cache::DatabaseError>
        listActive(std::string_view accountId) const;

      private:
        cache::DatabaseConnection& m_connection;
        cache::IdentityRepository& m_repository;
    };
} // namespace javelin::jmap::identity
