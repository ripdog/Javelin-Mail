#pragma once

#include "protocol/ProtocolTypes.h"

#include <QString>

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace javelin::protocol
{
    struct CacheInstanceId
    {
        QUuid value;
        friend bool operator==(const CacheInstanceId&, const CacheInstanceId&) = default;
    };

    struct CacheSchemaVersion
    {
        std::uint32_t value = 1;
        friend bool operator==(const CacheSchemaVersion&, const CacheSchemaVersion&) = default;
    };

    struct CacheDataVersion
    {
        std::uint64_t value = 0;
        friend bool operator==(const CacheDataVersion&, const CacheDataVersion&) = default;
    };

    struct CacheIdentity
    {
        CacheInstanceId instance;
        CacheSchemaVersion schema;
        CacheDataVersion dataVersion;
        friend bool operator==(const CacheIdentity&, const CacheIdentity&) = default;
    };

    struct MailboxWindowMaterialization
    {
        QString accountId;
        QString mailboxId;
        std::uint64_t offset = 0;
        std::uint32_t limit = 0;
    };

    using Materialization = std::variant<MailboxWindowMaterialization>;

    struct MaterializationRequest
    {
        RequestId id;
        ScopeId scope;
        Materialization request;
    };

    struct CancelMaterializationScopeRequest
    {
        ScopeId scope;
    };

    struct MaterializationAccepted
    {
        RequestId id;
    };

    struct MaterializationRejected
    {
        RequestId id;
        BoundaryError error;
    };

    using MaterializationReply = std::variant<MaterializationAccepted, MaterializationRejected>;

    enum class CacheSuspendReason : std::uint8_t
    {
        Migration,
        Replacement,
        Recovery,
    };

    struct CacheAccessSuspendedAcknowledgement
    {
        CacheInstanceId instance;
    };

    struct CacheAccessSuspendRequested
    {
        CacheInstanceId instance;
        CacheSuspendReason reason = CacheSuspendReason::Recovery;
        std::optional<CacheSchemaVersion> targetSchema;
    };

    struct CacheAccessResumed
    {
        CacheIdentity cache;
        QString cacheDatabasePath;
        InvalidationEpoch epoch;
    };

    struct MailboxWindowInvalidation
    {
        QString mailboxId;
        std::uint64_t offset = 0;
        std::uint64_t limit = 0;
        std::optional<std::uint64_t> total;
    };

    struct SearchWindowInvalidation
    {
        QString queryKey;
        std::uint64_t offset = 0;
        std::uint64_t limit = 0;
        std::optional<std::uint64_t> total;
    };

    struct CacheInvalidation
    {
        InvalidationEpoch epoch;
        std::vector<ChangedDomain> changedDomains;
        std::vector<QString> affectedKeys;
        QString accountId{};
        bool optimisticProjection = false;
        std::vector<QString> mailboxIds{};
        std::vector<QString> messageContentEmailIds{};
        std::vector<MailboxWindowInvalidation> mailboxWindows{};
        std::vector<SearchWindowInvalidation> searchWindows{};
    };

    struct ThreadMaterializationProgress
    {
        QString accountId;
        std::vector<QString> threadIds;
        bool inFlight = false;
        bool success = true;
        QString error;
    };

    class MaterializationClient
    {
      public:
        virtual ~MaterializationClient() = default;
        [[nodiscard]] virtual MaterializationReply
        requestMaterialization(MaterializationRequest request) = 0;
        virtual void cancelMaterializationScope(ScopeId scope) = 0;
    };

    class CacheAccessClient
    {
      public:
        virtual ~CacheAccessClient() = default;
        [[nodiscard]] virtual std::optional<BoundaryError>
        acknowledgeCacheAccessSuspended(CacheAccessSuspendedAcknowledgement acknowledgement) = 0;
    };
} // namespace javelin::protocol
