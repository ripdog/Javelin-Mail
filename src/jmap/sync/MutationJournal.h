#pragma once

#include "jmap/cache/Database.h"
#include "jmap/sync/ConsistencyDomain.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::sync
{

    enum class MutationStatus
    {
        Pending,
        InFlight,
        Accepted,
        Rejected,
        Unknown,
    };

    struct MutationRecord
    {
        std::string mutationId;
        std::optional<std::string> operationGroupId;
        ConsistencyDomain domain;
        std::string objectId;
        std::string mutationKind;
        MutationStatus status = MutationStatus::Pending;
        std::string payloadJson;
        std::optional<std::string> baseState;
        std::optional<std::string> acceptedState;
        std::optional<std::string> errorJson;
    };

    class MutationJournalRepository
    {
      public:
        explicit MutationJournalRepository(javelin::jmap::cache::DatabaseConnection& connection);

        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        put(const MutationRecord& record);
        [[nodiscard]] std::variant<std::optional<MutationRecord>,
                                   javelin::jmap::cache::DatabaseError>
        find(std::string_view mutationId) const;
        [[nodiscard]] std::variant<std::vector<MutationRecord>, javelin::jmap::cache::DatabaseError>
        listForObject(const ConsistencyDomain& domain, std::string_view objectId) const;
        [[nodiscard]] std::variant<std::vector<MutationRecord>, javelin::jmap::cache::DatabaseError>
        listByStatus(const ConsistencyDomain& domain, MutationStatus status,
                     std::size_t limit) const;
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        transition(std::string_view mutationId, MutationStatus status,
                   std::optional<std::string_view> acceptedState = std::nullopt,
                   std::optional<std::string_view> errorJson = std::nullopt);
        [[nodiscard]] std::variant<std::size_t, javelin::jmap::cache::DatabaseError>
        recoverInFlight();
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        remove(std::string_view mutationId);

      private:
        javelin::jmap::cache::DatabaseConnection& m_connection;
    };

    [[nodiscard]] std::string_view toString(MutationStatus status);
    [[nodiscard]] std::optional<MutationStatus> mutationStatusFromString(std::string_view value);
    [[nodiscard]] bool projectsOptimistically(MutationStatus status);

} // namespace javelin::jmap::sync
