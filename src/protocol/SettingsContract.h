#pragma once

#include "protocol/ProtocolTypes.h"

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace javelin::protocol
{
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

    struct CalendarDefaultDestination
    {
        QString ownerAccountId;
        QString accountId;
        QString calendarId;
        friend bool operator==(const CalendarDefaultDestination&,
                               const CalendarDefaultDestination&) = default;
    };

    struct WorkspaceSettings
    {
        std::uint32_t formatVersion = 1;
        QByteArray mainWindowState;
        bool composeRichTextDefault = true;
        CalendarDefaultDestination defaultCalendarDestination;
        std::vector<CalendarColorOverride> calendarColorOverrides;
        std::vector<QString> emailContextMenuLayout;
        std::vector<QString> calendarEventContextMenuLayout;
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
        std::optional<bool> undoSendUsesDialog;
        std::optional<WorkspaceSettings> workspace;
    };

    struct UpdateSettingsRequest
    {
        SettingsRevision baseRevision;
        SettingsUpdate update;
    };

    struct SettingsSnapshot
    {
        SettingsRevision revision;
        std::uint32_t schemaVersion = 7;
        std::vector<AccountSettings> accounts;
        std::vector<MailboxSelectionSettings> syncedMailboxSelections;
        std::vector<MailboxSelectionSettings> notificationMailboxSelections;
        std::vector<QString> remoteContentSenders;
        std::vector<QString> remoteContentDomains;
        AppearanceSettings appearance;
        AttachmentSettings attachments;
        std::int32_t undoSendDelaySeconds = 10;
        bool undoSendUsesDialog = false;
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

    class SettingsClient
    {
      public:
        virtual ~SettingsClient() = default;
        [[nodiscard]] virtual SettingsReadReply getSettings() = 0;
        [[nodiscard]] virtual SettingsUpdateReply updateSettings(UpdateSettingsRequest request) = 0;
    };
} // namespace javelin::protocol
