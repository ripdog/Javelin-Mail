#pragma once

#include "jmap/calendar/CalendarReader.h"
#include "protocol/ProcessBoundary.h"

#include <QString>
#include <QStringList>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace javelin::app
{

    struct MailboxQueryWindowChange
    {
        QString mailboxId;
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

    struct MailCacheChange
    {
        QString accountId;
        QStringList mailboxIds;
        std::vector<MailboxQueryWindowChange> queryWindows;
        std::vector<SearchQueryWindowChange> searchWindows;
        QStringList messageContentEmailIds{};
        bool mailboxTreeChanged = false;
        bool hasNewMail = false;
        bool optimisticProjection = false;
        bool contactsChanged = false;
        bool identitiesChanged = false;
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
