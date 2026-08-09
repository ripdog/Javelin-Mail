#pragma once

#include "jmap/cache/Database.h"
#include "jmap/sync/MutationJournal.h"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{
    class MailboxRepository;
}

namespace javelin::jmap::sync
{
    struct MailboxSubscriptionMutationRecord
    {
        std::string mutationId;
        std::optional<std::string> operationGroupId;
        std::string accountId;
        std::string mailboxId;
        MutationStatus status = MutationStatus::Pending;
        bool beforeSubscribed = true;
        bool afterSubscribed = true;
        std::optional<std::string> baseState;
        std::optional<std::string> acceptedState;
        std::optional<std::string> errorJson;
    };

    class MailboxMutationJournal
    {
      public:
        MailboxMutationJournal(javelin::jmap::cache::DatabaseConnection& connection,
                               javelin::jmap::cache::MailboxRepository& repository);

        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        queue(const MailboxSubscriptionMutationRecord& record);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        transition(const MailboxSubscriptionMutationRecord& record, MutationStatus status,
                   std::optional<std::string_view> acceptedState = std::nullopt,
                   std::optional<std::string_view> errorJson = std::nullopt);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        reject(const MailboxSubscriptionMutationRecord& record,
               std::optional<std::string_view> acceptedState = std::nullopt,
               std::optional<std::string_view> errorJson = std::nullopt);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        accept(const MailboxSubscriptionMutationRecord& record, std::string_view state);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        reconcile(const MailboxSubscriptionMutationRecord& record, bool serverSubscribed,
                  std::string_view state);
        [[nodiscard]] std::variant<std::vector<MailboxSubscriptionMutationRecord>,
                                   javelin::jmap::cache::DatabaseError>
        listActive(std::string_view accountId) const;
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        rebase(javelin::jmap::cache::DatabaseTransaction& transaction,
               std::string_view accountId) const;

      private:
        javelin::jmap::cache::DatabaseConnection& m_connection;
        javelin::jmap::cache::MailboxRepository& m_repository;
    };
} // namespace javelin::jmap::sync
