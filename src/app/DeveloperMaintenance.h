#pragma once

#include "storage/DatabaseError.h"

#include <QCoroTask>

#include <QString>
#include <QStringList>

#include <cstdint>
#include <variant>

namespace javelin::app
{
    enum class DeveloperMailboxCacheKind
    {
        Sqlite,
        Bodies,
        SqliteAndBodies,
    };

    enum class DeveloperOfflineClearPolicy
    {
        Preserve,
        Disable,
    };

    struct DeveloperMailboxClearCommand
    {
        QString accountId;
        QString mailboxId;
        DeveloperMailboxCacheKind kind = DeveloperMailboxCacheKind::Sqlite;
        DeveloperOfflineClearPolicy offlinePolicy = DeveloperOfflineClearPolicy::Preserve;
    };

    struct DeveloperMailboxClearSummary
    {
        QString accountId;
        QString mailboxId;
        QStringList invalidatedMailboxIds;
        DeveloperMailboxCacheKind kind = DeveloperMailboxCacheKind::Sqlite;
        std::uint64_t maintenanceGeneration = 0;
        std::uint64_t rowsDiscarded = 0;
        std::uint64_t projectionsRemoved = 0;
        std::uint64_t logicalBytesReleased = 0;
        std::uint64_t reclaimedBytes = 0;
        std::uint64_t deferredBytes = 0;
        bool offlineStorageDisabled = false;
    };

    using DeveloperMailboxClearExecutionResult =
        std::variant<DeveloperMailboxClearSummary, javelin::jmap::cache::DatabaseError>;

    struct DeveloperMailboxClearQueued
    {
        QString jobId;
    };

    using DeveloperMailboxClearResult =
        std::variant<DeveloperMailboxClearQueued, javelin::jmap::cache::DatabaseError>;

    class DeveloperMaintenancePort
    {
      public:
        virtual ~DeveloperMaintenancePort() = default;
        [[nodiscard]] virtual QCoro::Task<DeveloperMailboxClearResult>
        clearMailboxCache(DeveloperMailboxClearCommand command) = 0;
    };
} // namespace javelin::app
