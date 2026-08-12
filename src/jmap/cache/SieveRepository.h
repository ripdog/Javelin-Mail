#pragma once

#include "jmap/sieve/SieveTypes.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{
    class SieveRepository
    {
      public:
        explicit SieveRepository(DatabaseConnection& connection);

        [[nodiscard]] std::optional<DatabaseError>
        replaceAll(std::string_view accountId,
                   const std::vector<javelin::jmap::sieve::SieveScript>& scripts,
                   std::string_view state);
        [[nodiscard]] std::optional<DatabaseError>
        replaceAll(DatabaseTransaction& transaction, std::string_view accountId,
                   const std::vector<javelin::jmap::sieve::SieveScript>& scripts,
                   std::string_view state);
        [[nodiscard]] std::optional<DatabaseError>
        project(DatabaseTransaction& transaction, std::string_view accountId,
                const std::vector<javelin::jmap::sieve::SieveScript>& scripts);
        [[nodiscard]]
        std::variant<std::vector<javelin::jmap::sieve::SieveScript>, DatabaseError>
        list(std::string_view accountId) const;
        [[nodiscard]] std::variant<std::optional<std::string>, DatabaseError>
        state(std::string_view accountId) const;

      private:
        DatabaseConnection& m_connection;
    };
} // namespace javelin::jmap::cache
