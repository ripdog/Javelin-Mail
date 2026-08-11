#pragma once

#include "jmap/JmapCore.h"

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
        explicit EmailMutationBatchSubmitter(javelin::jmap::JmapCore& core);

        [[nodiscard]] QCoro::Task<EmailMutationBatchSubmission>
        submit(javelin::jmap::LiveConnectionSettings settings, std::string accountId,
               std::string operationGroupId, std::size_t batchLimit,
               std::function<void()> batchPrepared = {});

      private:
        javelin::jmap::JmapCore& m_core;
    };

} // namespace javelin::app
