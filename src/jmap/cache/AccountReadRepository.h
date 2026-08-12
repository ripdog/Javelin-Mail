#pragma once

#include "storage/sqlite/DatabaseConnection.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    struct CachedAccount
    {
        std::string accountId;
        std::string name;
        bool isPersonal = false;
        bool isReadOnly = false;
        bool isPrimary = false;
        bool hasMailCapability = false;
        bool mayCreateTopLevelMailbox = false;
        std::string ownerAccountId;
        bool hasSubmissionCapability = false;
        std::uint64_t maxDelayedSendSeconds = 0;
    };

    class AccountReader
    {
      public:
        virtual ~AccountReader() = default;

        [[nodiscard]] virtual std::variant<std::vector<CachedAccount>, DatabaseError>
        listAll() const = 0;
        [[nodiscard]] virtual std::variant<std::vector<CachedAccount>, DatabaseError>
        listOwnedBy(std::string_view ownerAccountId) const = 0;
        [[nodiscard]] virtual std::variant<std::optional<CachedAccount>, DatabaseError>
        findById(std::string_view accountId) const = 0;
    };

    class AccountReadRepository final : public AccountReader
    {
      public:
        explicit AccountReadRepository(ReadOnlyDatabaseConnection& connection);

        [[nodiscard]] std::variant<std::vector<CachedAccount>, DatabaseError>
        listAll() const override;
        [[nodiscard]] std::variant<std::vector<CachedAccount>, DatabaseError>
        listOwnedBy(std::string_view ownerAccountId) const override;
        [[nodiscard]] std::variant<std::optional<CachedAccount>, DatabaseError>
        findById(std::string_view accountId) const override;

      private:
        ReadOnlyDatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
