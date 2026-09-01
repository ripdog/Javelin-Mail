#include "app/CacheInvalidationPublisher.h"

#include <algorithm>
#include <ranges>
#include <utility>

namespace javelin::app
{
    namespace
    {
        constexpr std::size_t maximumAffectedKeys = 64;

        template <typename Container, typename Value>
        void appendUniqueBounded(Container& target, Value value)
        {
            if (static_cast<std::size_t>(target.size()) >= maximumAffectedKeys ||
                std::ranges::find(target, value) != target.end())
                return;
            target.push_back(std::move(value));
        }

        void appendMailboxWindowUnique(std::vector<MailboxQueryWindowChange>& target,
                                       MailboxQueryWindowChange value)
        {
            const auto found =
                std::ranges::find_if(target,
                                     [&value](const auto& existing)
                                     {
                                         return existing.mailboxId == value.mailboxId &&
                                                existing.offset == value.offset &&
                                                existing.limit == value.limit;
                                     });
            if (found == target.end())
                target.push_back(std::move(value));
            else if (value.total.has_value())
                found->total = value.total;
        }

        void appendSearchWindowUnique(std::vector<SearchQueryWindowChange>& target,
                                      SearchQueryWindowChange value)
        {
            const auto found =
                std::ranges::find_if(target,
                                     [&value](const auto& existing)
                                     {
                                         return existing.queryKey == value.queryKey &&
                                                existing.offset == value.offset &&
                                                existing.limit == value.limit;
                                     });
            if (found == target.end())
                target.push_back(std::move(value));
            else if (value.total.has_value())
                found->total = value.total;
        }
    } // namespace

    CacheInvalidationPublisher::CacheInvalidationPublisher(QObject* parent) : QObject(parent)
    {
        m_flushTimer.setSingleShot(true);
        connect(&m_flushTimer, &QTimer::timeout, this, &CacheInvalidationPublisher::flush);
    }

    void CacheInvalidationPublisher::publish(MailCacheChange change)
    {
        if (!m_pending.empty() && m_pending.back().accountId == change.accountId)
            merge(m_pending.back(), std::move(change));
        else
            m_pending.push_back(std::move(change));

        if (!m_flushTimer.isActive())
            m_flushTimer.start(0);
    }

    void CacheInvalidationPublisher::publishImmediately(MailCacheChange change)
    {
        publish(std::move(change));
        flush();
    }

    void CacheInvalidationPublisher::flush()
    {
        m_flushTimer.stop();
        while (!m_pending.empty())
        {
            auto change = std::move(m_pending.front());
            m_pending.pop_front();
            Q_EMIT invalidated(MailCacheInvalidation{
                .epoch = 0,
                .changedDomains = changedDomains(change),
                .affectedKeys = affectedKeys(change),
                .change = std::move(change),
            });
        }
    }

    void CacheInvalidationPublisher::merge(MailCacheChange& target, MailCacheChange source)
    {
        if (target.accountId.isEmpty())
            target.accountId = source.accountId;
        for (auto& mailboxId : source.mailboxIds)
        {
            if (!target.mailboxIds.contains(mailboxId))
                target.mailboxIds.push_back(std::move(mailboxId));
        }
        for (auto& window : source.queryWindows)
            appendMailboxWindowUnique(target.queryWindows, std::move(window));
        for (auto& window : source.searchWindows)
            appendSearchWindowUnique(target.searchWindows, std::move(window));
        for (auto& emailId : source.messageContentEmailIds)
            appendUniqueBounded(target.messageContentEmailIds, std::move(emailId));
        target.mailboxTreeChanged = target.mailboxTreeChanged || source.mailboxTreeChanged;
        target.emailObjectsChanged = target.emailObjectsChanged || source.emailObjectsChanged;
        target.optimisticProjection = target.optimisticProjection || source.optimisticProjection;
        target.mailTagsChanged = target.mailTagsChanged || source.mailTagsChanged;
        target.contactsChanged = target.contactsChanged || source.contactsChanged;
        target.identitiesChanged = target.identitiesChanged || source.identitiesChanged;
    }

    std::vector<javelin::protocol::ChangedDomain>
    CacheInvalidationPublisher::changedDomains(const MailCacheChange& change)
    {
        std::vector<javelin::protocol::ChangedDomain> domains;
        if (change.mailboxTreeChanged)
            domains.push_back(javelin::protocol::ChangedDomain::MailboxTree);
        if (!change.mailboxIds.empty() || !change.queryWindows.empty() ||
            !change.searchWindows.empty())
            domains.push_back(javelin::protocol::ChangedDomain::MailQueryWindows);
        if (change.emailObjectsChanged || change.optimisticProjection)
            domains.push_back(javelin::protocol::ChangedDomain::MessageMetadata);
        if (change.mailTagsChanged)
            domains.push_back(javelin::protocol::ChangedDomain::MailTags);
        if (!change.messageContentEmailIds.empty())
            domains.push_back(javelin::protocol::ChangedDomain::MessageContent);
        if (change.contactsChanged)
            domains.push_back(javelin::protocol::ChangedDomain::Contacts);
        if (change.identitiesChanged)
            domains.push_back(javelin::protocol::ChangedDomain::SenderIdentities);
        return domains;
    }

    std::vector<QString> CacheInvalidationPublisher::affectedKeys(const MailCacheChange& change)
    {
        std::vector<QString> keys;
        appendUniqueBounded(keys, change.accountId);
        for (const auto& mailboxId : change.mailboxIds)
            appendUniqueBounded(keys, mailboxId);
        for (const auto& window : change.queryWindows)
            appendUniqueBounded(keys, window.mailboxId);
        for (const auto& window : change.searchWindows)
            appendUniqueBounded(keys, window.queryKey);
        for (const auto& emailId : change.messageContentEmailIds)
            appendUniqueBounded(keys, emailId);
        return keys;
    }
} // namespace javelin::app
