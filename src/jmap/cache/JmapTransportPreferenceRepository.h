#pragma once

#include "jmap/cache/Database.h"

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace javelin::jmap::cache
{
    struct JmapTransportTarget
    {
        std::string ownerAccountId;
        std::string webSocketUrl;
    };

    using JmapTransportTargetResult =
        std::variant<std::optional<JmapTransportTarget>, DatabaseError>;

    class JmapTransportPreferenceRepository
    {
      public:
        explicit JmapTransportPreferenceRepository(DatabaseConnection& connection);

        [[nodiscard]] JmapTransportTargetResult resolve(std::string_view accountId) const;

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
