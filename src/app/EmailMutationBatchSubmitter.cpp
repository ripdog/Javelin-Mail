#include "app/EmailMutationBatchSubmitter.h"

#include <iterator>
#include <utility>

namespace javelin::app
{

    EmailMutationBatchSubmitter::EmailMutationBatchSubmitter(javelin::jmap::JmapCore& core)
        : m_core(core)
    {
    }

    QCoro::Task<EmailMutationBatchSubmission>
    EmailMutationBatchSubmitter::submit(javelin::jmap::LiveConnectionSettings settings,
                                        std::string accountId, std::string operationGroupId,
                                        const std::size_t batchLimit,
                                        std::function<void()> batchPrepared)
    {
        EmailMutationBatchSubmission outcome{
            .submitted =
                {
                    .accountId = accountId,
                    .attemptedEmailCount = 0,
                    .updatedEmailCount = 0,
                    .failedEmailCount = 0,
                    .statePreconditionUsed = false,
                    .items = {},
                    .receipt = {},
                },
            .error = std::nullopt,
        };
        std::optional<std::string> nextEmailState;
        while (true)
        {
            if (batchPrepared)
                batchPrepared();
            auto batchResult = co_await m_core.submitPendingEmailMutations(
                settings, accountId, operationGroupId, batchLimit, nextEmailState);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&batchResult))
            {
                outcome.error = *error;
                co_return outcome;
            }

            auto batch = std::get<javelin::jmap::SubmittedEmailMutations>(std::move(batchResult));
            if (batch.attemptedEmailCount == 0)
                co_return outcome;

            auto& submitted = outcome.submitted;
            submitted.attemptedEmailCount += batch.attemptedEmailCount;
            submitted.updatedEmailCount += batch.updatedEmailCount;
            submitted.failedEmailCount += batch.failedEmailCount;
            submitted.statePreconditionUsed =
                submitted.statePreconditionUsed || batch.statePreconditionUsed;
            submitted.items.insert(submitted.items.end(),
                                   std::make_move_iterator(batch.items.begin()),
                                   std::make_move_iterator(batch.items.end()));
            submitted.receipt.acceptedObjectIds.insert(
                submitted.receipt.acceptedObjectIds.end(),
                std::make_move_iterator(batch.receipt.acceptedObjectIds.begin()),
                std::make_move_iterator(batch.receipt.acceptedObjectIds.end()));
            submitted.receipt.rejectedObjectIds.insert(
                submitted.receipt.rejectedObjectIds.end(),
                std::make_move_iterator(batch.receipt.rejectedObjectIds.begin()),
                std::make_move_iterator(batch.receipt.rejectedObjectIds.end()));
            submitted.receipt.incompleteMaterialization =
                submitted.receipt.incompleteMaterialization ||
                batch.receipt.incompleteMaterialization;
            submitted.receipt.affectedCacheViews = std::move(batch.receipt.affectedCacheViews);
            for (auto& domain : batch.receipt.domains)
            {
                if (domain.dataType != "Email")
                    continue;
                if (submitted.receipt.domains.empty())
                    submitted.receipt.domains.push_back(domain);
                else
                    submitted.receipt.domains.front().newState = domain.newState;
                if (batch.statePreconditionUsed)
                    nextEmailState = domain.newState;
            }

            if (batch.updatedEmailCount + batch.failedEmailCount < batch.attemptedEmailCount)
                co_return outcome;
        }
    }

} // namespace javelin::app
