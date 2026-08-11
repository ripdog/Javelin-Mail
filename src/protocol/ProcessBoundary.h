#pragma once

#include <QByteArray>
#include <QString>
#include <QUuid>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

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
        std::uint16_t minor = 9;

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

    struct InvalidationEpoch
    {
        std::uint64_t value = 0;

        friend bool operator==(const InvalidationEpoch&, const InvalidationEpoch&) = default;
    };

    struct SettingsRevision
    {
        std::uint64_t value = 0;

        friend bool operator==(const SettingsRevision&, const SettingsRevision&) = default;
    };

    struct CacheIdentity
    {
        CacheInstanceId instance;
        CacheSchemaVersion schema;
        CacheDataVersion dataVersion;

        friend bool operator==(const CacheIdentity&, const CacheIdentity&) = default;
    };

    struct HelloRequest
    {
        ProtocolVersion protocol;
        BuildIdentity build;
    };

    struct RefreshAccountCommand
    {
        QString accountId;
        bool force = false;

        friend bool operator==(const RefreshAccountCommand&,
                               const RefreshAccountCommand&) = default;
    };

    enum class RemoteActionKind : std::uint16_t
    {
        RemoveConfiguredAccount,
        CalendarReadCached,
        CalendarReadAccounts,
        CalendarReadCalendars,
        CalendarRequestRange,
        CalendarCreateEvent,
        CalendarUpdateEvent,
        CalendarDeleteEvent,
        CalendarSetDefault,
        CalendarCreate,
        CalendarDelete,
        CalendarSetVisible,
        ComposeOpen,
        ComposeLoadSenderIdentities,
        ComposeSaveDraft,
        ComposeSend,
        ComposeLoadWorkingCopy,
        ComposeStoreWorkingCopy,
        ComposeDiscard,
        ContactRequestRefresh,
        ContactMutateAddressBook,
        ContactSave,
        ContactSetStarred,
        ContactDelete,
        ContactCreateGroup,
        ContactDeleteGroup,
        ContactSetGroupMembership,
        ContactCopy,
        ContactImport,
        ContactMerge,
        ContactUploadMedia,
        ContactDownloadMedia,
        MailQueueMailboxMutation,
        MailQueueDestroy,
        MailQueueMarkUnread,
        MailQueueMarkRead,
        MailQueueSetFlagged,
        MailSubmitPending,
        SieveList,
        SieveGet,
        SieveValidate,
        SieveSave,
        SieveDelete,
        SieveActivate,
        IdentityList,
        IdentitySave,
        IdentityDelete,
        AccountBootstrap,
        MessageContent,
        AttachmentDownload,
        MessageSource,
        MailboxObserve,
        MailboxUnobserve,
        MailboxWindow,
        SearchWindow,
        SearchRetire,
        Undo,
        Redo,
        UndoAcknowledgeRemove,
        UndoForget,
        UndoSnapshot,
        ReloadSettings,
        WorkPause,
        WorkResume,
        WorkRetry,
        WorkList,
        WorkSummary,
        OnboardingDiscover,
        OnboardingStartOAuth,
        OnboardingFinishOAuth,
        OnboardingAuthenticateManually,
        OnboardingRevokeOAuth,
        OnboardingCancelOAuth,
        CalendarSetSubscribed,
        DeveloperDiagnosticsSnapshot,
        DeveloperMailboxClear,
        AcknowledgeRemoteActionResult,
        DeveloperLogSetSubscribed,
        DeveloperLogClear,
        CalendarRespondEvent,
        MailQueueSetTag,
        MailSaveTagDefinition,
        MailDeleteTag,
        MailSetMailboxSubscribed,
        MailCreateMailbox,
        MailDestroyMailbox,
        ComposeScheduleSend,
        MailQueueSetSelectionFlagged,
        ThreadEnsure,
        Last = ThreadEnsure,
    };

    struct RemoteActionCommand
    {
        RemoteActionKind kind = RemoteActionKind::RemoveConfiguredAccount;
        QByteArray payload;

        friend bool operator==(const RemoteActionCommand&, const RemoteActionCommand&) = default;
    };

    using ApplicationCommand = std::variant<RefreshAccountCommand, RemoteActionCommand>;

    struct CommandRequest
    {
        CommandId id;
        ApplicationCommand command;
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

    struct GetSettingsRequest
    {
    };

    struct AccountSettings
    {
        QString id;
        std::uint64_t revision = 0;
        QString displayName;
        QString sessionUrl;
        QString loginEmail;
        QString tokenEndpoint;
        QString oauthClientId;
        QString oauthIssuer = {};
        QString oauthResource = {};
        QString oauthScope = {};
        QString revocationEndpoint = {};
        QString registrationClientUri = {};
        bool hasCredentials = false;
        QString credentialHandle = {};
        qint64 tokenExpiresAtEpochSeconds = 0;
        bool reauthenticationRequired = false;
        std::vector<QString> cachedAccountIds;

        friend bool operator==(const AccountSettings&, const AccountSettings&) = default;
    };

    struct MailboxSelectionSettings
    {
        QString accountId;
        std::vector<QString> mailboxIds;

        friend bool operator==(const MailboxSelectionSettings&,
                               const MailboxSelectionSettings&) = default;
    };

    struct AppearanceSettings
    {
        std::int32_t messageColorMode = 0;

        friend bool operator==(const AppearanceSettings&, const AppearanceSettings&) = default;
    };

    struct AttachmentSettings
    {
        bool alwaysAsk = true;
        QString directory;

        friend bool operator==(const AttachmentSettings&, const AttachmentSettings&) = default;
    };

    struct CalendarColorOverride
    {
        QString calendarId;
        QString color;

        friend bool operator==(const CalendarColorOverride&,
                               const CalendarColorOverride&) = default;
    };

    struct WorkspaceSettings
    {
        std::uint32_t formatVersion = 1;
        QByteArray mainWindowState;
        bool composeRichTextDefault = true;
        std::vector<CalendarColorOverride> calendarColorOverrides;

        friend bool operator==(const WorkspaceSettings&, const WorkspaceSettings&) = default;
    };

    struct SettingsUpdate
    {
        std::optional<std::vector<AccountSettings>> accounts;
        std::optional<std::vector<MailboxSelectionSettings>> syncedMailboxSelections;
        std::optional<std::vector<MailboxSelectionSettings>> notificationMailboxSelections;
        std::optional<std::vector<QString>> remoteContentSenders;
        std::optional<std::vector<QString>> remoteContentDomains;
        std::optional<AppearanceSettings> appearance;
        std::optional<AttachmentSettings> attachments;
        std::optional<std::int32_t> undoSendDelaySeconds;
        std::optional<WorkspaceSettings> workspace;
    };

    struct UpdateSettingsRequest
    {
        SettingsRevision baseRevision;
        SettingsUpdate update;
    };

    struct CacheAccessSuspendedAcknowledgement
    {
        CacheInstanceId instance;
    };

    struct PingRequest
    {
    };

    using ClientRequest =
        std::variant<HelloRequest, CommandRequest, MaterializationRequest,
                     CancelMaterializationScopeRequest, GetSettingsRequest, UpdateSettingsRequest,
                     CacheAccessSuspendedAcknowledgement, PingRequest>;

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

    struct ReadyReply
    {
        ProtocolVersion protocol;
        DaemonInstanceId daemon;
        CacheIdentity cache;
        QString cacheDatabasePath;
        InvalidationEpoch epoch;
        SettingsRevision settingsRevision;
    };

    struct HandshakeRejected
    {
        BoundaryError error;
    };

    using HandshakeReply = std::variant<ReadyReply, HandshakeRejected>;

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

    struct CommandAccepted
    {
        CommandId id;
        std::optional<OperationId> operation;
        InvalidationEpoch epoch;
        std::vector<ChangedDomain> changedDomains;
        std::vector<QString> affectedKeys;
        std::optional<QByteArray> immediateResult;
    };

    struct CommandRejected
    {
        CommandId id;
        BoundaryError error;
    };

    using CommandReply = std::variant<CommandAccepted, CommandRejected>;

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

    struct SettingsSnapshot
    {
        SettingsRevision revision;
        std::uint32_t schemaVersion = 5;
        std::vector<AccountSettings> accounts;
        std::vector<MailboxSelectionSettings> syncedMailboxSelections;
        std::vector<MailboxSelectionSettings> notificationMailboxSelections;
        std::vector<QString> remoteContentSenders;
        std::vector<QString> remoteContentDomains;
        AppearanceSettings appearance;
        AttachmentSettings attachments;
        std::int32_t undoSendDelaySeconds = 10;
        WorkspaceSettings workspace;

        friend bool operator==(const SettingsSnapshot&, const SettingsSnapshot&) = default;
    };

    struct SettingsSnapshotReply
    {
        SettingsSnapshot snapshot;
    };

    struct SettingsReadRejected
    {
        BoundaryError error;
    };

    using SettingsReadReply = std::variant<SettingsSnapshotReply, SettingsReadRejected>;

    struct SettingsUpdated
    {
        SettingsRevision revision;
    };

    struct SettingsUpdateRejected
    {
        SettingsRevision currentRevision;
        BoundaryError error;
    };

    using SettingsUpdateReply = std::variant<SettingsUpdated, SettingsUpdateRejected>;

    struct OperationFailed
    {
        OperationId operation;
        BoundaryError error;
    };

    struct OperationCompleted
    {
        OperationId operation;
        QByteArray result;
    };

    enum class AccountState : std::uint8_t
    {
        Unknown,
        Ready,
        Synchronizing,
        AuthenticationRequired,
        Failed,
        Paused,
    };

    struct AccountStatus
    {
        QString accountId;
        AccountState state = AccountState::Unknown;
        QString detail;
    };

    enum class DaemonLifecycle : std::uint8_t
    {
        Starting,
        Ready,
        Recovering,
        ShuttingDown,
    };

    struct DaemonStatus
    {
        DaemonLifecycle lifecycle = DaemonLifecycle::Starting;
        std::vector<AccountStatus> accounts;
    };

    struct OpenMailboxRoute
    {
        QString accountId;
        QString mailboxId;
        QString activationToken;
    };

    struct OpenMessageRoute
    {
        QString accountId;
        QString mailboxId;
        QString mailboxName;
        QString threadId;
        QString emailId;
        QString activationToken;
    };

    struct OpenComposeRoute
    {
        QString composeSessionId;
        QString activationToken;
    };

    struct OpenSettingsRoute
    {
        QString connectionId;
        QString activationToken;
    };

    struct RestoreDraftRoute
    {
        QString accountId;
        QString draftEmailId;
        QString composeSessionId;
        QString activationToken;
    };

    struct OpenTaskCenterRoute
    {
        QString activationToken;
    };

    struct RaiseGuiRoute
    {
        QString activationToken;
    };

    struct OpenMailtoRoute
    {
        QString uri;
        QString activationToken;
    };

    using ActivationRoute =
        std::variant<OpenMailboxRoute, OpenMessageRoute, OpenComposeRoute, RaiseGuiRoute,
                     OpenSettingsRoute, RestoreDraftRoute, OpenTaskCenterRoute, OpenMailtoRoute>;

    enum class CacheSuspendReason : std::uint8_t
    {
        Migration,
        Replacement,
        Recovery,
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

    struct DaemonShutdownRequested
    {
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

    struct ActivationRequested
    {
        ActivationRoute route;
    };

    struct DaemonStatusChanged
    {
        DaemonStatus status;
    };

    struct DiagnosticLogEntry
    {
        std::uint64_t timestampMilliseconds = 0;
        std::uint8_t level = 0;
        QString subsystem;
        QString message;
    };

    struct DaemonLogEntries
    {
        std::vector<DiagnosticLogEntry> entries;
    };

    using BoundaryEvent =
        std::variant<CacheInvalidation, OperationFailed, OperationCompleted, SettingsUpdated,
                     ActivationRequested, DaemonStatusChanged, CacheAccessSuspendRequested,
                     CacheAccessResumed, DaemonShutdownRequested, DaemonLogEntries,
                     ThreadMaterializationProgress>;

    struct BoundaryLimits
    {
        std::size_t maximumStringBytes = 4096;
        std::size_t maximumCollectionItems = 256;
        std::size_t maximumAffectedKeys = 64;
        std::size_t maximumMaterializationItems = 500;
        std::size_t maximumWorkspaceBytes = 8 * 1024 * 1024;
        std::size_t maximumFrameBytes = 1024 * 1024;
    };

    [[nodiscard]] std::optional<BoundaryError> validate(const ClientRequest& request,
                                                        const BoundaryLimits& limits = {});

    [[nodiscard]] std::size_t estimatedEncodedSize(const ClientRequest& request);
    [[nodiscard]] std::size_t estimatedEncodedSize(const BoundaryEvent& event);

    class BoundaryEventSink
    {
      public:
        virtual ~BoundaryEventSink() = default;
        virtual void onBoundaryEvent(const BoundaryEvent& event) = 0;
    };

    class ActivationRequestHandler
    {
      public:
        virtual ~ActivationRequestHandler() = default;
        [[nodiscard]] virtual std::optional<BoundaryError>
        handleGuiActivation(const ActivationRoute& route) = 0;
    };

    class DaemonRequestHandler : public ActivationRequestHandler
    {
      public:
        ~DaemonRequestHandler() override = default;

        [[nodiscard]] virtual HandshakeReply handleHello(const HelloRequest& request) = 0;
        [[nodiscard]] virtual CommandReply handleCommand(CommandRequest request) = 0;
        [[nodiscard]] virtual MaterializationReply
        handleMaterialization(MaterializationRequest request) = 0;
        virtual void
        handleCancelMaterializationScope(const CancelMaterializationScopeRequest& request) = 0;
        [[nodiscard]] virtual SettingsReadReply
        handleGetSettings(const GetSettingsRequest& request) = 0;
        [[nodiscard]] virtual SettingsUpdateReply
        handleUpdateSettings(UpdateSettingsRequest request) = 0;
        [[nodiscard]] virtual std::optional<BoundaryError>
        handleCacheAccessSuspended(const CacheAccessSuspendedAcknowledgement& acknowledgement) = 0;
        [[nodiscard]] virtual std::optional<BoundaryError>
        handlePing(const PingRequest& request) = 0;
        [[nodiscard]] virtual std::optional<BoundaryError> handleGuiReadyForActivation() = 0;
        [[nodiscard]] std::optional<BoundaryError>
        handleGuiActivation(const ActivationRoute&) override
        {
            return BoundaryError{.code = BoundaryErrorCode::UnsupportedOperation,
                                 .field = QStringLiteral("activation"),
                                 .detail = QStringLiteral("GUI activation is not supported")};
        }
    };

    class CommandClient
    {
      public:
        virtual ~CommandClient() = default;
        [[nodiscard]] virtual CommandReply submitCommand(CommandRequest request) = 0;
    };

    class MaterializationClient
    {
      public:
        virtual ~MaterializationClient() = default;
        [[nodiscard]] virtual MaterializationReply
        requestMaterialization(MaterializationRequest request) = 0;
        virtual void cancelMaterializationScope(ScopeId scope) = 0;
    };

    class SettingsClient
    {
      public:
        virtual ~SettingsClient() = default;
        [[nodiscard]] virtual SettingsReadReply getSettings() = 0;
        [[nodiscard]] virtual SettingsUpdateReply updateSettings(UpdateSettingsRequest request) = 0;
    };

    class DaemonStatusClient
    {
      public:
        virtual ~DaemonStatusClient() = default;
        [[nodiscard]] virtual HandshakeReply hello(HelloRequest request) = 0;
        [[nodiscard]] virtual std::optional<BoundaryError> ping() = 0;
    };

    class ActivationClient
    {
      public:
        virtual ~ActivationClient() = default;
        [[nodiscard]] virtual std::optional<BoundaryError> readyForActivation() = 0;
    };

    class CacheAccessClient
    {
      public:
        virtual ~CacheAccessClient() = default;
        [[nodiscard]] virtual std::optional<BoundaryError>
        acknowledgeCacheAccessSuspended(CacheAccessSuspendedAcknowledgement acknowledgement) = 0;
    };

} // namespace javelin::protocol
