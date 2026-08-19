#pragma once

#include <QString>
#include <QUuid>

#include <cstddef>
#include <cstdint>

namespace javelin::protocol
{
    struct CommandId
    {
        QUuid value;
        friend bool operator==(const CommandId&, const CommandId&) = default;
    };

    struct RequestId
    {
        QUuid value;
        friend bool operator==(const RequestId&, const RequestId&) = default;
    };

    struct ScopeId
    {
        QUuid value;
        friend bool operator==(const ScopeId&, const ScopeId&) = default;
    };

    struct OperationId
    {
        QUuid value;
        friend bool operator==(const OperationId&, const OperationId&) = default;
    };

    struct ProtocolVersion
    {
        std::uint16_t major = 5;
        std::uint16_t minor = 13;
        friend bool operator==(const ProtocolVersion&, const ProtocolVersion&) = default;
    };

    struct BuildIdentity
    {
        QString application;
        QString revision;
        friend bool operator==(const BuildIdentity&, const BuildIdentity&) = default;
    };

    struct DaemonInstanceId
    {
        QUuid value;
        friend bool operator==(const DaemonInstanceId&, const DaemonInstanceId&) = default;
    };

    struct SettingsRevision
    {
        std::uint64_t value = 0;
        friend bool operator==(const SettingsRevision&, const SettingsRevision&) = default;
    };

    struct InvalidationEpoch
    {
        std::uint64_t value = 0;
        friend bool operator==(const InvalidationEpoch&, const InvalidationEpoch&) = default;
    };

    enum class BoundaryErrorCode : std::uint8_t
    {
        InvalidRequest,
        InvalidIdentifier,
        ValueTooLarge,
        TooManyValues,
        InvalidProtocol,
        UnsupportedOperation,
        Busy,
        StaleSettingsRevision,
        MissingObject,
        NoUsableAccountConfiguration,
        CacheUnavailable,
        DaemonShuttingDown,
        IncompatibleBuild,
        SettingsStorageFailure,
        SettingsMigrationFailure,
        TransportUnavailable,
        ProtocolViolation,
    };

    struct BoundaryError
    {
        BoundaryErrorCode code = BoundaryErrorCode::InvalidRequest;
        QString field;
        QString detail;
    };

    enum class ChangedDomain : std::uint8_t
    {
        MailboxTree,
        MailQueryWindows,
        MessageMetadata,
        MessageContent,
        Contacts,
        Calendars,
        SenderIdentities,
        History,
        BackgroundJobs,
        UserVisibleFailures,
    };

    struct BoundaryLimits
    {
        std::size_t maximumStringBytes = 4096;
        std::size_t maximumCollectionItems = 256;
        std::size_t maximumAffectedKeys = 64;
        std::size_t maximumMaterializationItems = 500;
        std::size_t maximumWorkspaceBytes = 8 * 1024 * 1024;
        std::size_t maximumFrameBytes = 1024 * 1024;
    };
} // namespace javelin::protocol
