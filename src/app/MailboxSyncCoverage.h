#pragma once

#include "storage/sqlite/DatabaseConnection.h"

#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace javelin::app
{

    [[nodiscard]] std::variant<bool, javelin::jmap::cache::DatabaseError>
    hasAuthoritativeCanonicalMailboxCoverage(
        javelin::jmap::cache::DatabaseConnection& databaseConnection, std::string_view accountId,
        std::span<const std::string> mailboxIds);

} // namespace javelin::app
