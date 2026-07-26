#pragma once

#include "jmap/EmailMutation.h"
#include "jmap/JmapCore.h"

#include <QCoroTask>

#include <optional>
#include <string>
#include <vector>

namespace javelin::app::undo
{

    class MailHistoryPort
    {
      public:
        virtual ~MailHistoryPort() = default;

        [[nodiscard]] virtual QCoro::Task<javelin::jmap::AuthoritativeEmailsResult>
        getAuthoritativeEmails(std::string accountId, std::vector<std::string> emailIds) = 0;
        [[nodiscard]] virtual javelin::jmap::QueuedEmailMutationResult
        queueExactEmailMutation(std::string accountId,
                                javelin::jmap::EmailMailboxMutation mutation) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::SubmittedEmailMutationsResult>
        submitPendingEmailMutations(std::string accountId,
                                    std::optional<std::string> operationGroupId = std::nullopt) = 0;
    };

} // namespace javelin::app::undo
