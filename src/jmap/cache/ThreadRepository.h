#pragma once

#include "jmap/cache/Database.h"
#include "jmap/domain/MailEntities.h"

#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    class ThreadRepository
    {
      public:
        explicit ThreadRepository(DatabaseConnection& connection);

        [[nodiscard]] std::optional<DatabaseError>
        replaceAll(std::string_view accountId,
                   const std::vector<javelin::jmap::domain::Thread>& threads);
        [[nodiscard]] std::optional<DatabaseError>
        upsertMany(std::string_view accountId,
                   const std::vector<javelin::jmap::domain::Thread>& threads);
        [[nodiscard]] std::variant<std::optional<javelin::jmap::domain::Thread>, DatabaseError>
        find(std::string_view accountId, std::string_view threadId) const;

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
