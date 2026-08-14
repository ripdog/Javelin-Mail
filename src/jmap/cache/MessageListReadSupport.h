#pragma once

#include "jmap/cache/MessageListReadTypes.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <optional>
#include <string_view>
#include <vector>

namespace javelin::jmap::cache::detail
{
    [[nodiscard]] std::optional<DatabaseError>
    attachMessageListMetadata(const DatabaseReadView& connection, std::string_view accountId,
                              std::vector<MessageListItem>& items);
}
