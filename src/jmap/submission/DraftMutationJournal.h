#pragma once

#include "jmap/domain/MailEntities.h"
#include "jmap/submission/ComposeTypes.h"
#include "jmap/sync/MutationJournal.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <optional>
#include <string>

namespace javelin::jmap::submission
{
    struct DraftMutationGroup
    {
        std::string operationGroupId;
        std::string createMutationId;
        std::optional<std::string> destroyMutationId;
        std::string accountId;
        std::string temporaryEmailId;
        std::optional<std::string> replacedEmailId;
        std::optional<javelin::jmap::domain::Email> baseEmail;
        javelin::jmap::domain::Email projectedEmail;
        DraftSnapshot baseSnapshot;
    };

    class DraftMutationJournal
    {
      public:
        explicit DraftMutationJournal(cache::DatabaseConnection& connection);

        [[nodiscard]] std::optional<cache::DatabaseError> queue(const DraftMutationGroup& group);
        [[nodiscard]] std::optional<cache::DatabaseError>
        transition(const DraftMutationGroup& group, sync::MutationStatus status);
        [[nodiscard]] std::optional<cache::DatabaseError>
        transitionDestruction(const DraftMutationGroup& group, sync::MutationStatus status);
        [[nodiscard]] std::optional<cache::DatabaseError>
        rejectCreation(const DraftMutationGroup& group,
                       std::optional<std::string_view> errorJson = std::nullopt);
        [[nodiscard]] std::optional<cache::DatabaseError>
        acceptCreation(const DraftMutationGroup& group,
                       const javelin::jmap::domain::Email& acceptedEmail,
                       const DraftSnapshot& snapshot, std::string_view acceptedState);
        [[nodiscard]] std::optional<cache::DatabaseError>
        acceptDestruction(const DraftMutationGroup& group, std::string_view acceptedState);
        [[nodiscard]] std::optional<cache::DatabaseError>
        rejectDestruction(const DraftMutationGroup& group,
                          std::optional<std::string_view> acceptedState,
                          std::optional<std::string_view> errorJson = std::nullopt);
        [[nodiscard]] std::variant<bool, cache::DatabaseError>
        hasActiveForCompose(std::string_view accountId, std::string_view composeSessionId) const;

      private:
        cache::DatabaseConnection& m_connection;
    };
} // namespace javelin::jmap::submission
