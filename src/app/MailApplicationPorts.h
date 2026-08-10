#pragma once

#include "app/MailApplicationTypes.h"
#include "app/MailboxSelectionMutation.h"
#include "app/MessageSelection.h"
#include "jmap/JmapCore.h"

#include <QCoroTask>

#include <QString>

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace javelin::app
{

    class MailCacheChangePublisher
    {
      public:
        virtual ~MailCacheChangePublisher() = default;
        virtual void publishCacheChange(MailCacheChange change) = 0;
    };

    struct QueuedMailboxSelectionMutation
    {
        std::string accountId;
        std::size_t queuedEmailCount = 0;
        std::size_t skippedEmailCount = 0;
        std::vector<javelin::jmap::QueuedEmailMutation> queuedMutations;
        std::optional<QString> historyEntryId;
    };

    using QueuedMailboxSelectionMutationResult =
        std::variant<QueuedMailboxSelectionMutation, javelin::jmap::OperationError>;

    struct QueuedMessageSelectionMutation
    {
        std::string accountId;
        std::size_t queuedEmailCount = 0;
        std::vector<javelin::jmap::QueuedEmailMutation> queuedMutations;
        std::optional<QString> historyEntryId;
    };

    using QueuedMessageSelectionMutationResult =
        std::variant<QueuedMessageSelectionMutation, javelin::jmap::OperationError>;

    struct MailTagDefinition
    {
        std::string accountId;
        std::string keyword;
        std::string displayName;
        std::string color;
        int sortOrder = 0;

        friend bool operator==(const MailTagDefinition&, const MailTagDefinition&) = default;
    };

    struct SaveMailTagDefinition
    {
        std::string accountId;
        std::optional<std::string> keyword;
        std::string displayName;
        std::string color;
    };

    using SaveMailTagDefinitionResult =
        std::variant<MailTagDefinition, javelin::jmap::OperationError>;

    struct QueuedMailTagDeletion
    {
        std::string accountId;
        std::string keyword;
        std::string jobId;
    };

    using QueuedMailTagDeletionResult =
        std::variant<QueuedMailTagDeletion, javelin::jmap::OperationError>;

    // The GUI raises mail intents through this port. The implementation owns optimistic
    // projection, mutation grouping, and remote submission; presentation code only renders the
    // typed result and never assembles a protocol mutation itself.
    class MailCommandPort
    {
      public:
        virtual ~MailCommandPort() = default;

        [[nodiscard]] virtual QCoro::Task<QueuedMailboxSelectionMutationResult>
        queueMailboxSelectionMutation(MailboxSelectionMutationIntent intent) = 0;
        [[nodiscard]] virtual QCoro::Task<QueuedMessageSelectionMutationResult>
        queueDestroyMessages(std::string accountId, std::optional<std::string> sourceMailboxId,
                             MessageSelection selection) = 0;
        [[nodiscard]] virtual QCoro::Task<QueuedMessageSelectionMutationResult>
        queueMarkMessagesUnread(std::string accountId, std::optional<std::string> sourceMailboxId,
                                MessageSelection selection) = 0;
        [[nodiscard]] virtual QCoro::Task<QueuedMessageSelectionMutationResult>
        queueMarkEmailRead(std::string accountId, std::string emailId) = 0;
        [[nodiscard]] virtual QCoro::Task<QueuedMessageSelectionMutationResult>
        queueSetMessagesFlagged(std::string accountId, std::optional<std::string> sourceMailboxId,
                                MessageSelection selection, bool flagged) = 0;
        [[nodiscard]] virtual QCoro::Task<QueuedMessageSelectionMutationResult>
        queueSetMessagesTag(std::string accountId, std::optional<std::string> sourceMailboxId,
                            MessageSelection selection, std::string keyword, bool enabled) = 0;
        [[nodiscard]] virtual QCoro::Task<SaveMailTagDefinitionResult>
        saveTagDefinition(SaveMailTagDefinition definition) = 0;
        [[nodiscard]] virtual QCoro::Task<QueuedMailTagDeletionResult>
        deleteTag(std::string accountId, std::string keyword) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::MailboxSubscriptionChangeResult>
        setMailboxSubscribed(std::string accountId, std::string mailboxId, bool subscribed) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::MailboxDestroyResult>
        destroyMailbox(std::string accountId, std::string mailboxId) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::SubmittedEmailMutationsResult>
        submitPendingEmailMutations(std::string accountId,
                                    std::optional<std::string> operationGroupId = std::nullopt) = 0;
    };

} // namespace javelin::app
