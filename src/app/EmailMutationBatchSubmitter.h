#pragma once

#include "jmap/sync/EmailMutationEngine.h"

#include <QCoroTask>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace javelin::app
{

    struct EmailMutationBatchSubmission
    {
        javelin::jmap::SubmittedEmailMutations submitted;
        std::optional<javelin::jmap::OperationError> error;
    };

    class EmailMutationBatchSubmitter
    {
      public:
        explicit EmailMutationBatchSubmitter(javelin::jmap::EmailMutationEngine& mutationEngine);

        [[nodiscard]] QCoro::Task<EmailMutationBatchSubmission>
        submit(javelin::jmap::LiveConnectionSettings settings, std::string accountId,
               std::string operationGroupId, std::size_t batchLimit,
               std::function<void()> batchPrepared = {});

      private:
        javelin::jmap::EmailMutationEngine& m_mutationEngine;
    };

} // namespace javelin::app
