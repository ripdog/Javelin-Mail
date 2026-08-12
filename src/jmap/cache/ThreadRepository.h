#pragma once

#include "jmap/domain/MailEntities.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    enum class ThreadMembershipFreshness
    {
        Current,
        Stale,
    };

    struct ThreadMembershipRecord
    {
        javelin::jmap::domain::Thread thread;
        ThreadMembershipFreshness freshness = ThreadMembershipFreshness::Current;
        std::size_t globalMemberCount = 0;
        std::optional<std::string> state;
    };

    struct ThreadCoverage
    {
        ThreadMembershipFreshness freshness = ThreadMembershipFreshness::Current;
        std::size_t globalMemberCount = 0;
        std::size_t materializedMemberCount = 0;
        bool childEmailsComplete = false;
    };

    class ThreadRepository
    {
      public:
        explicit ThreadRepository(DatabaseConnection& connection);

        [[nodiscard]] std::optional<DatabaseError>
        replaceAll(std::string_view accountId,
                   const std::vector<javelin::jmap::domain::Thread>& threads,
                   std::optional<std::string_view> state = std::nullopt);
        [[nodiscard]] std::optional<DatabaseError>
        upsertMany(std::string_view accountId,
                   const std::vector<javelin::jmap::domain::Thread>& threads,
                   std::optional<std::string_view> state = std::nullopt);
        [[nodiscard]] std::optional<DatabaseError>
        upsertMany(DatabaseTransaction& transaction, std::string_view accountId,
                   const std::vector<javelin::jmap::domain::Thread>& threads,
                   std::optional<std::string_view> state = std::nullopt);
        [[nodiscard]] std::optional<DatabaseError>
        markStale(std::string_view accountId, std::span<const std::string> threadIds);
        [[nodiscard]] std::optional<DatabaseError>
        markStale(DatabaseTransaction& transaction, std::string_view accountId,
                  std::span<const std::string> threadIds);
        [[nodiscard]] std::variant<std::optional<javelin::jmap::domain::Thread>, DatabaseError>
        find(std::string_view accountId, std::string_view threadId) const;
        [[nodiscard]] std::variant<std::optional<ThreadMembershipRecord>, DatabaseError>
        findMembership(std::string_view accountId, std::string_view threadId) const;
        [[nodiscard]] std::variant<std::optional<std::string>, DatabaseError>
        findThreadIdByEmailId(std::string_view accountId, std::string_view emailId) const;
        [[nodiscard]] std::variant<std::vector<std::string>, DatabaseError>
        missingEmailIds(std::string_view accountId, std::string_view threadId,
                        std::size_t limit) const;
        [[nodiscard]] std::variant<std::optional<ThreadCoverage>, DatabaseError>
        coverage(std::string_view accountId, std::string_view threadId) const;
        [[nodiscard]] std::variant<std::optional<std::size_t>, DatabaseError>
        countMailboxMembersIfComplete(std::string_view accountId, std::string_view mailboxId,
                                      std::string_view threadId) const;

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
