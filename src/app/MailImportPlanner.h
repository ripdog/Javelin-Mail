#pragma once

#include "app/MailImportRepository.h"

#include <variant>
#include <vector>

namespace javelin::app
{
    struct MailImportScanPlan
    {
        std::vector<MailImportMailboxRecord> mailboxes;
        std::vector<MailImportItemRecord> items;
    };

    using MailImportScanPlanResult =
        std::variant<MailImportScanPlan, javelin::jmap::OperationError>;

    [[nodiscard]] MailImportScanPlanResult
    planMailImportSources(const MailImportOperationRecord& operation);
} // namespace javelin::app
