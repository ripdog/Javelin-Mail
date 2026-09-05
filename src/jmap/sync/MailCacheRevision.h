#pragma once

#include "storage/DatabaseError.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace javelin::jmap::sync
{
    struct MailCacheRevisionFence
    {
        std::string accountId;
        std::uint64_t revision = 0;
    };

    class MailCacheRevisionRepository final
    {
      public:
        explicit MailCacheRevisionRepository(
            javelin::jmap::cache::DatabaseConnection& databaseConnection);

        [[nodiscard]] std::variant<MailCacheRevisionFence, javelin::jmap::cache::DatabaseError>
        capture(std::string accountId) const;
        [[nodiscard]] std::variant<bool, javelin::jmap::cache::DatabaseError>
        isCurrent(const MailCacheRevisionFence& fence) const;
        [[nodiscard]] std::variant<bool, javelin::jmap::cache::DatabaseError>
        advanceIfCurrent(javelin::jmap::cache::DatabaseTransaction& transaction,
                         const MailCacheRevisionFence& fence) const;
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        advance(javelin::jmap::cache::DatabaseTransaction& transaction,
                std::string_view accountId) const;

      private:
        [[nodiscard]] std::variant<std::uint64_t, javelin::jmap::cache::DatabaseError>
        read(std::string_view accountId) const;

        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
    };
} // namespace javelin::jmap::sync
