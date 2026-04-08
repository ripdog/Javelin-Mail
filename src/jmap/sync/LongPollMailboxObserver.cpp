#include "jmap/sync/LongPollMailboxObserver.h"

#include <algorithm>

namespace javelin::jmap::sync
{

    LongPollMailboxObserver::LongPollMailboxObserver(
        AbstractLongPollMailboxRefresher& refresher,
        AbstractRefreshNotificationSink& notificationSink, std::string accountId,
        std::string mailboxId)
        : m_refresher(refresher), m_notificationSink(notificationSink),
          m_accountId(std::move(accountId)), m_mailboxId(std::move(mailboxId))
    {
    }

    QCoro::Task<void> LongPollMailboxObserver::onUpdate(const LongPollResponse& response)
    {
        const bool affectsMail =
            std::ranges::find(response.changedTypes, std::string{"Email"}) !=
                response.changedTypes.end() ||
            std::ranges::find(response.changedTypes, std::string{"Mailbox"}) !=
                response.changedTypes.end();
        if (!affectsMail)
        {
            co_return;
        }

        const auto refreshResult =
            co_await m_refresher.refreshMailbox(m_accountId, m_mailboxId);
        if (const auto* error = std::get_if<MailboxRefreshError>(&refreshResult))
        {
            static_cast<void>(error);
            co_return;
        }

        const auto& summary = std::get<MailboxRefreshSummary>(refreshResult);
        if (!summary.notificationCandidates.empty())
        {
            m_notificationSink.publish(m_accountId, m_mailboxId,
                                       summary.notificationCandidates);
        }
    }

} // namespace javelin::jmap::sync
