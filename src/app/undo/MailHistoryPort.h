#pragma once

#include "jmap/EmailMutation.h"
#include "jmap/sync/EmailMutationEngine.h"

#include <QCoroTask>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace javelin::app::undo
{

    class MailHistoryPort
    {
      public:
        virtual ~MailHistoryPort() = default;

        [[nodiscard]] virtual javelin::jmap::AuthoritativeEmailsResult
        getEffectiveEmails(std::string_view accountId, std::span<const std::string> emailIds) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::AuthoritativeEmailsResult>
        getAuthoritativeEmails(std::string accountId, std::vector<std::string> emailIds) = 0;
        [[nodiscard]] virtual javelin::jmap::QueuedEmailMutationResult
        queueExactEmailMutation(std::string accountId,
                                javelin::jmap::EmailMailboxMutation mutation) = 0;
        [[nodiscard]] virtual javelin::jmap::QueuedEmailMutationsResult
        queueExactEmailMutations(std::string accountId,
                                 std::vector<javelin::jmap::EmailMailboxMutation> mutations) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::SubmittedEmailMutationsResult>
        submitPendingEmailMutations(std::string accountId,
                                    std::optional<std::string> operationGroupId = std::nullopt) = 0;
    };

} // namespace javelin::app::undo
