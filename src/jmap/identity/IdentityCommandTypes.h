#pragma once

#include "jmap/OperationError.h"
#include "jmap/cache/IdentityReader.h"
#include "jmap/domain/MailEntities.h"

#include <variant>
#include <vector>

namespace javelin::jmap::identity
{
    struct IdentitySnapshot
    {
        std::vector<javelin::jmap::domain::Identity> identities;
        std::vector<javelin::jmap::cache::PendingIdentityCreate> pendingCreates;
    };

    using IdentityListResult = std::variant<IdentitySnapshot, OperationError>;
    using IdentitySaveResult = std::variant<javelin::jmap::domain::Identity, OperationError>;
    using IdentityDeleteResult = std::variant<std::monostate, OperationError>;
} // namespace javelin::jmap::identity
