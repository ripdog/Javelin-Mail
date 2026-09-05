#pragma once

#include "jmap/OperationError.h"

#include <QCoroTask>

#include <string>
#include <variant>

namespace javelin::app
{
    struct CanonicalMailboxRefreshSummary
    {
        bool cacheChanged = false;
    };

    using CanonicalMailboxRefreshResult =
        std::variant<CanonicalMailboxRefreshSummary, javelin::jmap::OperationError>;

    class MailQueryRefreshPort
    {
      public:
        virtual ~MailQueryRefreshPort() = default;

        [[nodiscard]] virtual QCoro::Task<CanonicalMailboxRefreshResult>
        refreshCanonicalMailbox(std::string accountId, std::string mailboxId) = 0;
    };
} // namespace javelin::app
