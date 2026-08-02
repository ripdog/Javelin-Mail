#include "app/GuiMailApplicationEvents.h"

#include "app/GuiDaemonSession.h"

#include <algorithm>
#include <ranges>

namespace javelin::app
{
    namespace
    {
        [[nodiscard]] MailAccountStatus mailStatus(const javelin::protocol::AccountState state)
        {
            using State = javelin::protocol::AccountState;
            switch (state)
            {
            case State::Ready:
                return MailAccountStatus::Connected;
            case State::Synchronizing:
                return MailAccountStatus::Connecting;
            case State::AuthenticationRequired:
                return MailAccountStatus::AuthenticationPaused;
            case State::Unknown:
            case State::Failed:
            case State::Paused:
                return MailAccountStatus::Disconnected;
            }
            return MailAccountStatus::Disconnected;
        }

        [[nodiscard]] bool hasDomain(const std::vector<javelin::protocol::ChangedDomain>& domains,
                                     const javelin::protocol::ChangedDomain domain)
        {
            return std::ranges::contains(domains, domain);
        }
    } // namespace

    GuiMailApplicationEvents::GuiMailApplicationEvents(GuiDaemonSession& session, QObject* parent)
        : MailApplicationEventsPort(parent), m_session(session)
    {
        connect(&m_session, &GuiDaemonSession::cacheInvalidated, this,
                &GuiMailApplicationEvents::publishInvalidation);
        connect(&m_session, &GuiDaemonSession::daemonStatusChanged, this,
                &GuiMailApplicationEvents::applyStatus);
        connect(&m_session, &GuiDaemonSession::recoveryStarted, this,
                [this](const QString&)
                {
                    applyStatus({.lifecycle = javelin::protocol::DaemonLifecycle::Recovering,
                                 .accounts = {}});
                });
        if (m_session.daemonStatus().has_value())
            applyStatus(*m_session.daemonStatus());
    }

    std::unordered_map<std::string, MailAccountStatus>
    GuiMailApplicationEvents::accountStatuses() const
    {
        return m_statuses;
    }

    void GuiMailApplicationEvents::applyStatus(const javelin::protocol::DaemonStatus& status)
    {
        std::unordered_map<std::string, MailAccountStatus> next;
        for (const auto& account : status.accounts)
        {
            const auto id = account.accountId.toStdString();
            const auto value = mailStatus(account.state);
            next.emplace(id, value);
            const auto current = m_statuses.find(id);
            if (current == m_statuses.end() || current->second != value)
                Q_EMIT accountStatusChanged(account.accountId, value);
        }
        for (const auto& [id, oldStatus] : m_statuses)
        {
            Q_UNUSED(oldStatus)
            if (!next.contains(id))
                Q_EMIT accountStatusChanged(QString::fromStdString(id),
                                            MailAccountStatus::Disconnected);
        }
        m_statuses = std::move(next);
    }

    void GuiMailApplicationEvents::publishInvalidation(
        const javelin::protocol::CacheInvalidation& invalidation)
    {
        MailCacheChange change;
        if (!invalidation.affectedKeys.empty())
        {
            change.accountId = invalidation.affectedKeys.front();
            for (auto key = std::next(invalidation.affectedKeys.begin());
                 key != invalidation.affectedKeys.end(); ++key)
            {
                if (!key->isEmpty() && !change.mailboxIds.contains(*key))
                    change.mailboxIds.push_back(*key);
            }
        }
        change.mailboxTreeChanged =
            hasDomain(invalidation.changedDomains, javelin::protocol::ChangedDomain::MailboxTree);
        change.hasNewMail = hasDomain(invalidation.changedDomains,
                                      javelin::protocol::ChangedDomain::MessageMetadata);
        change.optimisticProjection = change.hasNewMail;
        change.contactsChanged =
            hasDomain(invalidation.changedDomains, javelin::protocol::ChangedDomain::Contacts);
        Q_EMIT cacheInvalidated({
            .epoch = invalidation.epoch.value,
            .changedDomains = invalidation.changedDomains,
            .affectedKeys = invalidation.affectedKeys,
            .change = std::move(change),
        });
    }
} // namespace javelin::app
