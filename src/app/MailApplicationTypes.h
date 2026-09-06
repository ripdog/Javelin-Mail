#pragma once

#include "jmap/calendar/CalendarReader.h"
#include "protocol/ProtocolTypes.h"

#include <QString>
#include <QStringList>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace javelin::app
{

    struct MailboxQueryWindowChange
    {
        QString mailboxId;
        QString queryKey;
        std::size_t offset = 0;
        std::size_t limit = 0;
        std::optional<std::size_t> total;
    };

    struct SearchQueryWindowChange
    {
        QString queryKey;
        std::size_t offset = 0;
        std::size_t limit = 0;
        std::optional<std::size_t> total;
    };

    struct MailDependencyScope
    {
        static constexpr std::size_t maximumMailboxIds = 64;

        bool accountWide = false;
        QStringList mailboxIds{};

        [[nodiscard]] bool empty() const
        {
            return !accountWide && mailboxIds.isEmpty();
        }

        void addMailbox(QString mailboxId)
        {
            if (accountWide || mailboxId.isEmpty() || mailboxIds.contains(mailboxId))
                return;
            if (static_cast<std::size_t>(mailboxIds.size()) >= maximumMailboxIds)
            {
                accountWide = true;
                mailboxIds.clear();
                return;
            }
            mailboxIds.push_back(std::move(mailboxId));
        }

        void merge(const MailDependencyScope& other)
        {
            if (accountWide)
                return;
            if (other.accountWide)
            {
                accountWide = true;
                mailboxIds.clear();
                return;
            }
            for (const auto& mailboxId : other.mailboxIds)
                addMailbox(mailboxId);
        }
    };

    struct MailBackgroundEffects
    {
        MailDependencyScope offlineCatchUp{};
        bool rawSourceAvailabilityChanged = false;
        bool mailboxCountsChanged = false;
        bool vaultProjectionWorkQueued = false;
    };

    struct MailCacheChange
    {
        QString accountId;
        QStringList mailboxIds;
        std::vector<MailboxQueryWindowChange> queryWindows;
        std::vector<SearchQueryWindowChange> searchWindows;
        QStringList messageContentEmailIds{};
        bool mailboxTreeChanged = false;
        // Signals that committed Email rows changed. This drives metadata invalidation and
        // indexing; it deliberately says nothing about whether the change represents newly arrived
        // mail.
        bool emailObjectsChanged = false;
        bool optimisticProjection = false;
        bool mailTagsChanged = false;
        bool contactsChanged = false;
        bool identitiesChanged = false;
        MailBackgroundEffects background{};
    };

    struct MailCacheInvalidation
    {
        std::uint64_t epoch = 0;
        std::vector<javelin::protocol::ChangedDomain> changedDomains;
        std::vector<QString> affectedKeys;
        MailCacheChange change;
    };

    struct ThreadMaterializationProgress
    {
        QString accountId;
        QStringList threadIds;
        bool inFlight = false;
        bool success = true;
        QString error;
    };

    struct CalendarCacheChange
    {
        QString ownerAccountId;
        javelin::jmap::calendar::VisibleInterval interval;
        javelin::jmap::calendar::TimeZoneId displayTimeZone;
        std::size_t accountCount = 0;
        std::size_t eventCount = 0;
    };

} // namespace javelin::app
