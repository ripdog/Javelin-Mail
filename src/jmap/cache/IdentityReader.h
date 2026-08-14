#pragma once

#include "jmap/domain/MailEntities.h"
#include "storage/DatabaseError.h"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    struct PendingIdentityCreate
    {
        std::string creationId;
        std::string mutationId;
        javelin::jmap::domain::Identity identity;
        std::string status;
        std::optional<std::string> errorJson;
    };

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
        [[nodiscard]] virtual std::variant<std::vector<PendingIdentityCreate>, DatabaseError>
        listPendingCreates(std::string_view accountId) const = 0;
    };

} // namespace javelin::jmap::cache
