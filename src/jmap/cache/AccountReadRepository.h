#pragma once

#include "storage/sqlite/DatabaseConnection.h"

#include "jmap/cache/AccountIdentity.h"

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
        // Transitional name: accountId is the stable local/cache key, not necessarily the remote
        // JMAP account id. Consumers that need the wire id must use remoteAccountId/locator().
        std::string accountId;
        std::string connectionId;
        std::string remoteAccountId;
        std::string name;
        bool isPersonal = false;
        bool isReadOnly = false;
        bool isPrimary = false;
        bool hasMailCapability = false;
        bool mayCreateTopLevelMailbox = false;
        std::string ownerAccountId;
        bool hasSubmissionCapability = false;
        std::uint64_t maxDelayedSendSeconds = 0;

        [[nodiscard]] MailAccountKey key() const
        {
            return {.value = accountId};
        }

        [[nodiscard]] MailAccountLocator locator() const
        {
            return {.connectionId = connectionId, .remoteAccountId = remoteAccountId};
        }
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
        [[nodiscard]] virtual std::variant<std::optional<CachedAccount>, DatabaseError>
        findByLocator(const MailAccountLocator& locator) const;
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
