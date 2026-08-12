#pragma once

#include "jmap/sync/MailboxRefreshExecutor.h"
#include "jmap/sync/RefreshNotificationTypes.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::sync
{

    class RefreshNotificationPlanner
    {
      public:
        explicit RefreshNotificationPlanner(javelin::jmap::cache::DatabaseConnection& connection);

        [[nodiscard]] std::variant<std::vector<RefreshNotificationCandidate>,
                                   javelin::jmap::cache::DatabaseError>
        plan(std::string_view accountId, std::string_view mailboxId,
             const MailboxRefreshSummary& refreshSummary) const;

      private:
        javelin::jmap::cache::DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::sync
