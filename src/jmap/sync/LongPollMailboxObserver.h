#pragma once

#include "jmap/sync/LongPollWorker.h"
#include "jmap/sync/MailboxRefreshExecutor.h"

#include <QCoroTask>

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::sync
{

    class AbstractLongPollMailboxRefresher
    {
      public:
        virtual ~AbstractLongPollMailboxRefresher() = default;

        [[nodiscard]] virtual QCoro::Task<MailboxRefreshResult>
        refreshMailbox(std::string accountId, std::string mailboxId) = 0;
    };

    class AbstractRefreshNotificationSink
    {
      public:
        virtual ~AbstractRefreshNotificationSink() = default;

        virtual void publish(std::string_view accountId, std::string_view mailboxId,
                             const std::vector<RefreshNotificationCandidate>& candidates) = 0;
    };

    class LongPollMailboxObserver final : public AbstractLongPollObserver
    {
      public:
        LongPollMailboxObserver(AbstractLongPollMailboxRefresher& refresher,
                                AbstractRefreshNotificationSink& notificationSink,
                                std::string accountId, std::string mailboxId);

        [[nodiscard]] QCoro::Task<void> onUpdate(LongPollResponse response) override;

      private:
        AbstractLongPollMailboxRefresher& m_refresher;
        AbstractRefreshNotificationSink& m_notificationSink;
        std::string m_accountId;
        std::string m_mailboxId;
    };

} // namespace javelin::jmap::sync
