#pragma once

#include "app/MailTransferRepository.h"
#include "jmap/cache/AccountReadRepository.h"
#include "jmap/cache/MailboxReadRepository.h"
#include "jmap/domain/MailEntities.h"

#include <QString>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace javelin::app
{
    struct MailTransferIntent
    {
        std::string sourceAccountId; // Stable local/cache account key.
        std::optional<std::string> sourceMailboxId;
        std::string destinationAccountId; // Stable local/cache account key.
        std::string destinationMailboxId;
        MailTransferOperation operation = MailTransferOperation::Copy;
    };

    struct PlannedMailTransferItem
    {
        std::string sourceEmailId;
        std::string sourceBlobId;
        std::vector<std::string> sourceMailboxIds;
        std::vector<std::string> sourceKeywords;
        std::string sourceReceivedAt;
        std::uint64_t sourceSize = 0;
        std::vector<std::string> sourceRemoveMailboxIds;
        bool sourceDestroy = false;
    };

    struct PlannedMailTransfer
    {
        MailTransferTopology topology = MailTransferTopology::CrossServerImport;
        std::vector<PlannedMailTransferItem> items;
    };

    using MailTransferPlanResult = std::variant<PlannedMailTransfer, QString>;

    [[nodiscard]] MailTransferPlanResult planMailTransfer(
        const MailTransferIntent& intent, const std::vector<std::string>& emailIds,
        const std::vector<javelin::jmap::domain::Email>& emails,
        const std::vector<javelin::jmap::cache::MailboxTreeItem>& sourceMailboxes,
        const std::vector<javelin::jmap::cache::MailboxTreeItem>& destinationMailboxes,
        const javelin::jmap::cache::CachedAccount& sourceAccount,
        const javelin::jmap::cache::CachedAccount& destinationAccount);

} // namespace javelin::app
