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
                                                existing.queryKey == value.queryKey &&
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
            auto mailboxWindows = std::move(change.queryWindows);
            auto searchWindows = std::move(change.searchWindows);
            change.queryWindows.clear();
            change.searchWindows.clear();
            const auto maximum = javelin::protocol::BoundaryLimits{}.maximumCollectionItems;
            std::size_t mailboxOffset = 0;
            std::size_t searchOffset = 0;
            do
            {
                auto batch = change;
                // Leave room for bounded account, mailbox, content and affected-key fields.
                const auto byteBudget = javelin::protocol::BoundaryLimits{}.maximumFrameBytes / 8;
                std::size_t windowBytes = 0;
                while (mailboxOffset < mailboxWindows.size() && batch.queryWindows.size() < maximum)
                {
                    const auto& window = mailboxWindows[mailboxOffset];
                    const auto bytes = static_cast<std::size_t>(window.mailboxId.toUtf8().size() +
                                                                window.queryKey.toUtf8().size()) +
                                       32;
                    if (windowBytes + bytes > byteBudget && !batch.queryWindows.empty())
                        break;
                    windowBytes += bytes;
                    batch.queryWindows.push_back(std::move(mailboxWindows[mailboxOffset++]));
                }
                while (searchOffset < searchWindows.size() && batch.searchWindows.size() < maximum)
                {
                    const auto& window = searchWindows[searchOffset];
                    const auto bytes =
                        static_cast<std::size_t>(window.queryKey.toUtf8().size()) + 32;
                    if (windowBytes + bytes > byteBudget &&
                        (!batch.queryWindows.empty() || !batch.searchWindows.empty()))
                        break;
                    windowBytes += bytes;
                    batch.searchWindows.push_back(std::move(searchWindows[searchOffset++]));
                }
                Q_EMIT invalidated(MailCacheInvalidation{
                    .epoch = 0,
                    .changedDomains = changedDomains(batch),
                    .affectedKeys = affectedKeys(batch),
                    .change = std::move(batch),
                });
                change.background = {};
            } while (mailboxOffset < mailboxWindows.size() || searchOffset < searchWindows.size());
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
        target.background.offlineCatchUp.merge(source.background.offlineCatchUp);
        target.background.rawSourceAvailabilityChanged =
            target.background.rawSourceAvailabilityChanged ||
            source.background.rawSourceAvailabilityChanged;
        target.background.mailboxCountsChanged =
            target.background.mailboxCountsChanged || source.background.mailboxCountsChanged;
        target.background.vaultProjectionWorkQueued = target.background.vaultProjectionWorkQueued ||
                                                      source.background.vaultProjectionWorkQueued;
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
