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

    // A local cache commit landed while the server response was in flight, so the whole response
    // was discarded. The server and cache remain consistent; the caller owes a replacement refresh.
    struct CanonicalMailboxRefreshSuperseded
    {
    };

    using CanonicalMailboxRefreshResult =
        std::variant<CanonicalMailboxRefreshSummary, CanonicalMailboxRefreshSuperseded,
                     javelin::jmap::OperationError>;

    class MailQueryRefreshPort
    {
      public:
        virtual ~MailQueryRefreshPort() = default;

        [[nodiscard]] virtual QCoro::Task<CanonicalMailboxRefreshResult>
        refreshCanonicalMailbox(std::string accountId, std::string mailboxId) = 0;
    };
} // namespace javelin::app
