#pragma once

#include "storage/DatabaseError.h"

#include <QCoroTask>

#include <QString>

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace javelin::app
{

    struct DeveloperMailboxUsage
    {
        std::uint64_t sqliteEstimatedBytes = 0;
        std::uint64_t logicalBodyBytes = 0;
        std::uint64_t sharedBodyBytes = 0;
        std::uint64_t reclaimableBodyBytes = 0;
        std::uint64_t allocatedBodyBytes = 0;
        std::uint64_t missingBodyObjects = 0;
        std::uint64_t activeBodyLeases = 0;
    };

    struct DeveloperMailboxRecord
    {
        QString accountId;
        QString accountName;
        QString accountEmailAddress;
        QString mailboxId;
        QString mailboxName;
        std::optional<QString> parentMailboxId;
        std::optional<QString> parentMailboxName;
        std::optional<QString> role;
        std::uint64_t sortOrder = 0;
        std::uint64_t totalEmails = 0;
        std::uint64_t unreadEmails = 0;
        std::uint64_t totalThreads = 0;
        std::uint64_t unreadThreads = 0;
        bool isSubscribed = false;
        std::optional<QString> mailboxState;
        QString rawRightsJson;
        bool mayReadItems = false;
        bool mayAddItems = false;
        bool mayRemoveItems = false;
        bool maySetSeen = false;
        bool maySetKeywords = false;
        bool mayCreateChild = false;
        bool mayRename = false;
        bool mayDelete = false;
        bool maySubmit = false;
        std::optional<QString> accountMailboxState;
        std::optional<QString> accountEmailState;
        std::uint64_t cachedMembershipCount = 0;
        std::uint64_t queryWindowCount = 0;
        std::uint64_t queryWindowItemCount = 0;
        QString queryCoverageSummary;
        QString queryMaterializationSummary;
        std::optional<QString> oldestCachedMessage;
        std::optional<QString> newestCachedMessage;
        bool offlineDesired = false;
        std::optional<QString> offlineStatus;
        std::optional<QString> offlineQueryState;
        std::optional<QString> offlineEmailState;
        std::optional<std::uint64_t> offlineExpectedTotal;
        std::uint64_t offlineCompletedTotal = 0;
        std::uint64_t offlineCompletedBytes = 0;
        std::optional<std::uint64_t> offlineEstimatedBytes;
        std::uint64_t offlineGeneration = 0;
        std::optional<std::uint64_t> offlineCompletedGeneration;
        std::optional<QString> offlineAnchorEmailId;
        std::optional<QString> offlineLatestError;
        std::optional<QString> offlineUpdatedAt;
        std::uint64_t vaultReferenceCount = 0;
        std::uint64_t pendingVaultProjectionCount = 0;
        std::uint64_t failedVaultProjectionCount = 0;
        std::uint64_t completeVaultProjectionCount = 0;
        std::uint64_t activeMutationCount = 0;
        DeveloperMailboxUsage usage;
        QString measuredAt;
    };

    struct DeveloperDiagnosticsSnapshot
    {
        QString databasePath;
        QString vaultPath;
        std::uint64_t databaseDataVersion = 0;
        std::vector<DeveloperMailboxRecord> mailboxes;
    };

    using DeveloperDiagnosticsResult =
        std::variant<DeveloperDiagnosticsSnapshot, javelin::jmap::cache::DatabaseError>;

    class DeveloperDiagnosticsPort
    {
      public:
        virtual ~DeveloperDiagnosticsPort() = default;
        [[nodiscard]] virtual QCoro::Task<DeveloperDiagnosticsResult> snapshot() = 0;
    };

} // namespace javelin::app
