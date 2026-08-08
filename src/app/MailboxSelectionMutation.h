#pragma once

#include "app/MessageSelection.h"
#include "jmap/EmailMutation.h"
#include "jmap/cache/QueryService.h"
#include "jmap/domain/MailEntities.h"

#include <QString>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace javelin::app
{

    enum class MailboxSelectionOperation
    {
        Move,
        Copy,
        Archive,
        Junk,
        NotJunk,
    };

    struct MailboxSelectionMutationIntent
    {
        std::string accountId;
        MessageSelection selection;
        MailboxSelectionOperation operation = MailboxSelectionOperation::Move;
        std::optional<std::string> sourceMailboxId;
        std::optional<std::string> destinationMailboxId;
    };

    struct PlannedMailboxSelectionMutation
    {
        std::vector<javelin::jmap::EmailMailboxMutation> mutations;
        std::size_t skippedEmailCount = 0;
    };

    using MailboxSelectionMutationPlanResult =
        std::variant<PlannedMailboxSelectionMutation, QString>;

    [[nodiscard]] MailboxSelectionMutationPlanResult planMailboxSelectionMutation(
        const MailboxSelectionMutationIntent& intent, const std::vector<std::string>& emailIds,
        const std::vector<javelin::jmap::domain::Email>& emails,
        const std::vector<javelin::jmap::cache::MailboxTreeItem>& mailboxes);

} // namespace javelin::app
