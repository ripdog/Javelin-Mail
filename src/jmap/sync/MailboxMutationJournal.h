#pragma once

#include "jmap/domain/MailEntities.h"
#include "jmap/sync/MutationJournal.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <cstdint>
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

    struct MailboxCreateMutationRecord
    {
        std::string mutationId;
        std::optional<std::string> operationGroupId;
        std::string accountId;
        std::string creationId;
        std::string name;
        std::optional<std::string> parentId;
        std::uint64_t sortOrder = 0;
        bool isSubscribed = true;
        MutationStatus status = MutationStatus::Pending;
        std::optional<std::string> baseState;
        std::optional<std::string> acceptedState;
        std::optional<std::string> errorJson;
    };

    struct MailboxDestroyMutationRecord
    {
        std::string mutationId;
        std::optional<std::string> operationGroupId;
        std::string accountId;
        std::string mailboxId;
        MutationStatus status = MutationStatus::Pending;
        javelin::jmap::domain::Mailbox beforeMailbox;
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
        queue(const MailboxCreateMutationRecord& record);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        queue(const MailboxDestroyMutationRecord& record);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        transition(const MailboxSubscriptionMutationRecord& record, MutationStatus status,
                   std::optional<std::string_view> acceptedState = std::nullopt,
                   std::optional<std::string_view> errorJson = std::nullopt);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        transition(const MailboxCreateMutationRecord& record, MutationStatus status,
                   std::optional<std::string_view> acceptedState = std::nullopt,
                   std::optional<std::string_view> errorJson = std::nullopt);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        transition(const MailboxDestroyMutationRecord& record, MutationStatus status,
                   std::optional<std::string_view> acceptedState = std::nullopt,
                   std::optional<std::string_view> errorJson = std::nullopt);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        reject(const MailboxSubscriptionMutationRecord& record,
               std::optional<std::string_view> acceptedState = std::nullopt,
               std::optional<std::string_view> errorJson = std::nullopt);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        reject(const MailboxCreateMutationRecord& record,
               std::optional<std::string_view> acceptedState = std::nullopt,
               std::optional<std::string_view> errorJson = std::nullopt);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        accept(const MailboxCreateMutationRecord& record,
               const javelin::jmap::domain::Mailbox& mailbox, std::string_view state);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        reconcileCreated(const MailboxCreateMutationRecord& record,
                         const std::vector<javelin::jmap::domain::Mailbox>& mailboxes,
                         std::string_view state);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        retryCreateAtState(const MailboxCreateMutationRecord& record,
                           const std::vector<javelin::jmap::domain::Mailbox>& mailboxes,
                           std::string_view state);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        accept(const MailboxSubscriptionMutationRecord& record, std::string_view state);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        reject(const MailboxDestroyMutationRecord& record,
               std::optional<std::string_view> acceptedState = std::nullopt,
               std::optional<std::string_view> errorJson = std::nullopt,
               std::optional<javelin::jmap::domain::Mailbox> confirmedMailbox = std::nullopt);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        accept(const MailboxDestroyMutationRecord& record, std::string_view state);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        reconcileDestroyed(const MailboxDestroyMutationRecord& record, std::string_view state);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        reconcilePresent(const MailboxDestroyMutationRecord& record,
                         const javelin::jmap::domain::Mailbox& mailbox, std::string_view state);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        reconcile(const MailboxSubscriptionMutationRecord& record, bool serverSubscribed,
                  std::string_view state);
        [[nodiscard]] std::variant<std::vector<MailboxSubscriptionMutationRecord>,
                                   javelin::jmap::cache::DatabaseError>
        listActive(std::string_view accountId) const;
        [[nodiscard]] std::variant<std::vector<MailboxCreateMutationRecord>,
                                   javelin::jmap::cache::DatabaseError>
        listActiveCreates(std::string_view accountId) const;
        [[nodiscard]] std::variant<std::vector<MailboxDestroyMutationRecord>,
                                   javelin::jmap::cache::DatabaseError>
        listActiveDestroys(std::string_view accountId) const;
        [[nodiscard]] std::variant<bool, javelin::jmap::cache::DatabaseError>
        hasActive(std::string_view accountId) const;
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        rebase(javelin::jmap::cache::DatabaseTransaction& transaction,
               std::string_view accountId) const;

      private:
        javelin::jmap::cache::DatabaseConnection& m_connection;
        javelin::jmap::cache::MailboxRepository& m_repository;
    };
} // namespace javelin::jmap::sync
