#pragma once

#include "jmap/cache/Database.h"
#include "jmap/domain/MailEntities.h"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::sync
{

    enum class PendingActionStatus
    {
        Pending,
        InFlight,
        Failed,
    };

    struct PendingEmailPatch
    {
        std::string emailId;
        std::vector<std::string> addMailboxIds;
        std::vector<std::string> removeMailboxIds;
        std::vector<std::string> addKeywords;
        std::vector<std::string> removeKeywords;
    };

    struct PendingActionRecord
    {
        std::string pendingActionId;
        std::string accountId;
        PendingActionStatus status = PendingActionStatus::Pending;
        PendingEmailPatch emailPatch;
    };

    class PendingActionRepository
    {
      public:
        explicit PendingActionRepository(javelin::jmap::cache::DatabaseConnection& connection);

        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        put(const PendingActionRecord& record);
        [[nodiscard]]
        std::variant<std::vector<PendingActionRecord>, javelin::jmap::cache::DatabaseError>
        listForEmail(std::string_view accountId, std::string_view emailId) const;
        [[nodiscard]]
        std::variant<std::vector<PendingActionRecord>, javelin::jmap::cache::DatabaseError>
        listByStatus(std::string_view accountId, PendingActionStatus status,
                     std::size_t limit) const;
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        updateStatus(std::string_view pendingActionId, PendingActionStatus status);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        remove(std::string_view pendingActionId);

      private:
        javelin::jmap::cache::DatabaseConnection& m_connection;
    };

    [[nodiscard]] std::string_view toString(PendingActionStatus status);
    [[nodiscard]] std::optional<PendingActionStatus>
    pendingActionStatusFromString(std::string_view value);

    [[nodiscard]] javelin::jmap::domain::Email
    mergePendingEmailPatch(const javelin::jmap::domain::Email& email,
                           const std::vector<PendingActionRecord>& pendingActions);

} // namespace javelin::jmap::sync
