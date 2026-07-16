#pragma once

#include "jmap/domain/MailEntities.h"
#include "jmap/sync/MutationJournal.h"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::sync
{

    struct EmailPatchMutation
    {
        std::string emailId;
        std::vector<std::string> addMailboxIds;
        std::vector<std::string> removeMailboxIds;
        std::vector<std::string> addKeywords;
        std::vector<std::string> removeKeywords;
        bool destroy = false;
    };

    struct EmailMutationRecord
    {
        std::string mutationId;
        std::optional<std::string> operationGroupId;
        std::string accountId;
        MutationStatus status = MutationStatus::Pending;
        EmailPatchMutation patch;
        std::optional<std::string> baseState;
        std::optional<std::string> acceptedState;
        std::optional<std::string> errorJson;
    };

    class EmailMutationJournal
    {
      public:
        explicit EmailMutationJournal(javelin::jmap::cache::DatabaseConnection& connection);

        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        put(const EmailMutationRecord& record);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        queue(const EmailMutationRecord& record,
              const javelin::jmap::domain::Email& projectedEmail);
        [[nodiscard]]
        std::variant<std::vector<EmailMutationRecord>, javelin::jmap::cache::DatabaseError>
        listForEmail(std::string_view accountId, std::string_view emailId) const;
        [[nodiscard]]
        std::variant<std::vector<EmailMutationRecord>, javelin::jmap::cache::DatabaseError>
        listByStatus(std::string_view accountId, MutationStatus status, std::size_t limit) const;
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        transition(std::string_view mutationId, MutationStatus status,
                   std::optional<std::string_view> acceptedState = std::nullopt,
                   std::optional<std::string_view> errorJson = std::nullopt);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        remove(std::string_view mutationId);

      private:
        javelin::jmap::cache::DatabaseConnection& m_connection;
        MutationJournalRepository m_repository;
    };

    [[nodiscard]] javelin::jmap::domain::Email
    projectEmailMutations(const javelin::jmap::domain::Email& confirmedEmail,
                          const std::vector<EmailMutationRecord>& mutations);

} // namespace javelin::jmap::sync
