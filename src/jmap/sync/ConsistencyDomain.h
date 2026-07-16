#pragma once

#include "jmap/cache/Database.h"

#include <cstdint>
#include <string>
#include <variant>

namespace javelin::jmap::sync
{

    struct ConsistencyDomain
    {
        std::string accountId;
        std::string dataType;

        bool operator==(const ConsistencyDomain&) const = default;
    };

    struct RefreshFence
    {
        ConsistencyDomain domain;
        std::uint64_t mutationGeneration = 0;
    };

    class ConsistencyDomainRepository
    {
      public:
        explicit ConsistencyDomainRepository(javelin::jmap::cache::DatabaseConnection& connection);

        [[nodiscard]] std::variant<RefreshFence, javelin::jmap::cache::DatabaseError>
        captureRefresh(const ConsistencyDomain& domain) const;
        [[nodiscard]] std::variant<bool, javelin::jmap::cache::DatabaseError>
        isCurrent(const RefreshFence& fence) const;
        [[nodiscard]] std::variant<std::uint64_t, javelin::jmap::cache::DatabaseError>
        advanceMutation(const ConsistencyDomain& domain);
        [[nodiscard]] std::variant<std::uint64_t, javelin::jmap::cache::DatabaseError>
        mutationGeneration(const ConsistencyDomain& domain) const;

      private:
        javelin::jmap::cache::DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::sync
