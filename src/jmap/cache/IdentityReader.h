#pragma once

#include "jmap/cache/Database.h"
#include "jmap/domain/MailEntities.h"

#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    class IdentityReader
    {
      public:
        virtual ~IdentityReader() = default;

        [[nodiscard]] virtual std::variant<std::vector<javelin::jmap::domain::Identity>,
                                           DatabaseError>
        listByAccount(std::string_view accountId) const = 0;
        [[nodiscard]] virtual std::variant<std::optional<javelin::jmap::domain::Identity>,
                                           DatabaseError>
        find(std::string_view accountId, std::string_view identityId) const = 0;
    };

} // namespace javelin::jmap::cache
