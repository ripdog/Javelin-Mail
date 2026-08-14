#include "app/SettingsRepository.h"

#include "protocol/ProtocolValidation.h"

#include <QDataStream>
#include <QIODevice>
#include <QVariantMap>

#include <algorithm>
#include <utility>

namespace javelin::app
{

    namespace
    {
        constexpr auto schemaVersionKey = "settings/schemaVersion";
        constexpr auto revisionKey = "settings/revision";
        constexpr auto accountsGroup = "accounts";
        constexpr auto accountsSizeKey = "size";
        constexpr auto accountIdKey = "id";
        constexpr auto accountRevisionKey = "revision";
        constexpr auto accountDisplayNameKey = "displayName";
        constexpr auto accountSessionUrlKey = "sessionUrl";
        constexpr auto accountLoginEmailKey = "loginEmail";
        constexpr auto accountApiKeyKey = "apiKey";
        constexpr auto accountRefreshTokenKey = "refreshToken";
        constexpr auto accountTokenEndpointKey = "tokenEndpoint";
        constexpr auto accountOauthClientIdKey = "oauthClientId";
        constexpr auto accountOauthIssuerKey = "oauthIssuer";
        constexpr auto accountOauthResourceKey = "oauthResource";
        constexpr auto accountOauthScopeKey = "oauthScope";
        constexpr auto accountRevocationEndpointKey = "revocationEndpoint";
        constexpr auto accountRegistrationClientUriKey = "registrationClientUri";
        constexpr auto accountRegistrationAccessTokenKey = "registrationAccessToken";
        constexpr auto accountTokenExpiresAtKey = "tokenExpiresAt";
        constexpr auto accountReauthenticationRequiredKey = "reauthenticationRequired";
        constexpr auto accountCachedIdsKey = "cachedAccountIds";
        constexpr auto mailboxIdsKey = "mailboxIds";
        constexpr auto syncedMailboxGroup = "mailboxSync";
        constexpr auto notificationMailboxGroup = "mailboxNotifications";
        constexpr auto remoteContentGroup = "remoteContent";
        constexpr auto remoteContentSendersKey = "allowedSenders";
        constexpr auto remoteContentDomainsKey = "allowedDomains";
        constexpr auto appearanceGroup = "messageAppearance";
        constexpr auto appearanceColorModeKey = "colorMode";
        constexpr auto attachmentsGroup = "attachments";
        constexpr auto attachmentsAlwaysAskKey = "alwaysAsk";
        constexpr auto attachmentsDirectoryKey = "directory";
        constexpr auto composeUndoSendDelayKey = "compose/undoSendDelaySeconds";
        constexpr auto workspaceGroup = "workspace";
        constexpr auto workspaceFormatVersionKey = "formatVersion";
        constexpr auto workspaceWindowStateKey = "mainWindowState";
        constexpr auto workspaceComposeRichTextDefaultKey = "composeRichTextDefault";
        constexpr auto workspaceCalendarColorsKey = "calendarColorOverrides";
        constexpr auto workspaceCalendarIdKey = "calendarId";
        constexpr auto workspaceColorKey = "color";
        constexpr auto workspaceEmailContextMenuLayoutKey = "emailContextMenuLayout";
        constexpr auto legacyWindowGroup = "mainWindow";
        constexpr auto legacyCalendarColorsKey = "calendar/colorOverrides";
        constexpr int settingsSchemaVersion = 6;
        constexpr int workspaceFormatVersion = 1;
        constexpr int maximumAccounts = 256;
        constexpr int maximumSelections = 256;
        constexpr int maximumWorkspaceBytes = 8 * 1024 * 1024;

        [[nodiscard]] QString settingKey(const char* value)
        {
            return QString::fromLatin1(value);
        }

        [[nodiscard]] QString groupKey(const QString& accountId)
        {
            return accountId + QLatin1Char('/') + settingKey(mailboxIdsKey);
        }

        [[nodiscard]] SettingsRepositoryError qSettingsError(const SettingsRepositoryErrorCode code,
                                                             const QString& key,
                                                             const QSettings& settings)
        {
            return {.code = code,
                    .key = key,
                    .detail = QStringLiteral("QSettings could not access %1 (%2)")
                                  .arg(key, settings.fileName())};
        }

        [[nodiscard]] SettingsRepositoryError invalidValue(const QString& key,
                                                           const QString& detail)
        {
            return {.code = SettingsRepositoryErrorCode::InvalidStoredValue,
                    .key = key,
                    .detail = detail};
        }

        [[nodiscard]] std::vector<QString> toVector(const QStringList& values)
        {
            std::vector<QString> result;
            result.reserve(static_cast<std::size_t>(values.size()));
            for (const auto& value : values)
                result.push_back(value);
            return result;
        }

        [[nodiscard]] QStringList toStringList(const std::vector<QString>& values)
        {
            QStringList result;
            result.reserve(static_cast<qsizetype>(values.size()));
            for (const auto& value : values)
                result.push_back(value);
            return result;
        }

        [[nodiscard]] std::optional<SettingsRepositoryError>
        settingsStatusError(const QSettings& settings, const SettingsRepositoryErrorCode code,
                            const QString& key)
        {
            if (settings.status() == QSettings::NoError)
                return std::nullopt;
            return qSettingsError(code, key, settings);
        }

        [[nodiscard]] std::optional<SettingsRepositoryError>
        readSelections(QSettings& settings, const QString& group,
                       std::vector<javelin::protocol::MailboxSelectionSettings>& output)
        {
            settings.beginGroup(group);
            const auto keys = settings.allKeys();
            for (const auto& key : keys)
            {
                const QString suffix = QStringLiteral("/") + settingKey(mailboxIdsKey);
                if (!key.endsWith(suffix))
                    continue;
                const QString accountId = key.left(key.size() - suffix.size());
                if (accountId.isEmpty())
                {
                    settings.endGroup();
                    return invalidValue(group,
                                        QStringLiteral("mailbox selection has no account id"));
                }
                output.push_back({.accountId = accountId,
                                  .mailboxIds = toVector(settings.value(key).toStringList())});
            }
            settings.endGroup();
            std::ranges::sort(output, {}, &javelin::protocol::MailboxSelectionSettings::accountId);
            if (static_cast<int>(output.size()) > maximumSelections)
                return invalidValue(group, QStringLiteral("too many mailbox selections"));
            return std::nullopt;
        }

        void writeSelections(QSettings& settings, const QString& group,
                             const std::vector<javelin::protocol::MailboxSelectionSettings>& values)
        {
            settings.beginGroup(group);
            settings.remove(QString{});
            for (const auto& selection : values)
            {
                settings.setValue(groupKey(selection.accountId),
                                  toStringList(selection.mailboxIds));
            }
            settings.endGroup();
        }

        [[nodiscard]] std::variant<QByteArray, SettingsRepositoryError>
        encodeWorkspaceMap(const QVariantMap& values)
        {
            QByteArray encoded;
            QDataStream stream{&encoded, QIODeviceBase::WriteOnly};
            stream.setByteOrder(QDataStream::BigEndian);
            stream.setVersion(QDataStream::Qt_6_6);
            stream << values;
            if (stream.status() != QDataStream::Ok)
            {
                return SettingsRepositoryError{
                    .code = SettingsRepositoryErrorCode::MigrationFailed,
                    .key = settingKey(legacyWindowGroup),
                    .detail = QStringLiteral("could not encode legacy main-window state")};
            }
            return encoded;
        }

        [[nodiscard]] std::optional<SettingsRepositoryError>
        readWorkspace(QSettings& settings, javelin::protocol::WorkspaceSettings& workspace,
                      const bool includeLegacyWorkspace)
        {
            workspace.formatVersion = workspaceFormatVersion;
            if (includeLegacyWorkspace)
            {
                QVariantMap mainWindowValues;
                settings.beginGroup(settingKey(legacyWindowGroup));
                for (const auto& key : settings.allKeys())
                    mainWindowValues.insert(key, settings.value(key));
                settings.endGroup();
                if (!mainWindowValues.isEmpty())
                {
                    auto encoded = encodeWorkspaceMap(mainWindowValues);
                    if (const auto* error = std::get_if<SettingsRepositoryError>(&encoded))
                        return *error;
                    workspace.mainWindowState = std::get<QByteArray>(std::move(encoded));
                }

                const auto legacyColors =
                    settings.value(settingKey(legacyCalendarColorsKey)).toMap();
                workspace.calendarColorOverrides.reserve(
                    static_cast<std::size_t>(legacyColors.size()));
                for (auto it = legacyColors.cbegin(); it != legacyColors.cend(); ++it)
                {
                    const auto color = it.value().toString().trimmed();
                    if (!it.key().trimmed().isEmpty() && !color.isEmpty())
                    {
                        workspace.calendarColorOverrides.push_back(
                            {.calendarId = it.key().trimmed(), .color = color});
                    }
                }
                return std::nullopt;
            }

            settings.beginGroup(settingKey(workspaceGroup));
            bool ok = false;
            workspace.formatVersion =
                settings.value(settingKey(workspaceFormatVersionKey), workspaceFormatVersion)
                    .toUInt(&ok);
            if (!ok || workspace.formatVersion != workspaceFormatVersion)
            {
                settings.endGroup();
                return invalidValue(settingKey(workspaceFormatVersionKey),
                                    QStringLiteral("unsupported workspace format"));
            }
            workspace.mainWindowState =
                settings.value(settingKey(workspaceWindowStateKey)).toByteArray();
            workspace.composeRichTextDefault =
                settings.value(settingKey(workspaceComposeRichTextDefaultKey), true).toBool();
            workspace.emailContextMenuLayout = toVector(
                settings.value(settingKey(workspaceEmailContextMenuLayoutKey)).toStringList());
            if (workspace.mainWindowState.size() > maximumWorkspaceBytes)
            {
                settings.endGroup();
                return invalidValue(settingKey(workspaceWindowStateKey),
                                    QStringLiteral("workspace state is too large"));
            }
            const auto colorCount = settings.beginReadArray(settingKey(workspaceCalendarColorsKey));
            if (colorCount < 0 || colorCount > maximumSelections)
            {
                settings.endArray();
                settings.endGroup();
                return invalidValue(settingKey(workspaceCalendarColorsKey),
                                    QStringLiteral("invalid calendar color count"));
            }
            workspace.calendarColorOverrides.reserve(static_cast<std::size_t>(colorCount));
            for (int index = 0; index < colorCount; ++index)
            {
                settings.setArrayIndex(index);
                auto calendarId =
                    settings.value(settingKey(workspaceCalendarIdKey)).toString().trimmed();
                auto color = settings.value(settingKey(workspaceColorKey)).toString().trimmed();
                if (calendarId.isEmpty() || color.isEmpty())
                {
                    settings.endArray();
                    settings.endGroup();
                    return invalidValue(settingKey(workspaceCalendarColorsKey),
                                        QStringLiteral("calendar color entry is incomplete"));
                }
                workspace.calendarColorOverrides.push_back(
                    {.calendarId = std::move(calendarId), .color = std::move(color)});
            }
            settings.endArray();
            settings.endGroup();
            if (static_cast<int>(workspace.emailContextMenuLayout.size()) > maximumSelections)
                return invalidValue(settingKey(workspaceEmailContextMenuLayoutKey),
                                    QStringLiteral("too many context menu entries"));
            std::ranges::sort(workspace.calendarColorOverrides, {},
                              &javelin::protocol::CalendarColorOverride::calendarId);
            const auto duplicate =
                std::ranges::adjacent_find(workspace.calendarColorOverrides, {},
                                           &javelin::protocol::CalendarColorOverride::calendarId);
            if (duplicate != workspace.calendarColorOverrides.end())
            {
                return invalidValue(settingKey(workspaceCalendarColorsKey),
                                    QStringLiteral("calendar color ids must be unique"));
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<SettingsRepositoryError>
        applyUpdate(javelin::protocol::SettingsSnapshot& snapshot,
                    const javelin::protocol::SettingsUpdate& update)
        {
            if (update.accounts.has_value())
                snapshot.accounts = *update.accounts;
            if (update.syncedMailboxSelections.has_value())
                snapshot.syncedMailboxSelections = *update.syncedMailboxSelections;
            if (update.notificationMailboxSelections.has_value())
                snapshot.notificationMailboxSelections = *update.notificationMailboxSelections;
            if (update.remoteContentSenders.has_value())
                snapshot.remoteContentSenders = *update.remoteContentSenders;
            if (update.remoteContentDomains.has_value())
                snapshot.remoteContentDomains = *update.remoteContentDomains;
            if (update.appearance.has_value())
                snapshot.appearance = *update.appearance;
            if (update.attachments.has_value())
                snapshot.attachments = *update.attachments;
            if (update.undoSendDelaySeconds.has_value())
                snapshot.undoSendDelaySeconds = *update.undoSendDelaySeconds;
            if (update.workspace.has_value())
                snapshot.workspace = *update.workspace;

            if (snapshot.workspace.formatVersion != workspaceFormatVersion)
                return invalidValue(settingKey(workspaceFormatVersionKey),
                                    QStringLiteral("unsupported workspace format"));
            if (snapshot.workspace.mainWindowState.size() > maximumWorkspaceBytes)
                return invalidValue(settingKey(workspaceWindowStateKey),
                                    QStringLiteral("workspace state is too large"));
            for (auto& overrideValue : snapshot.workspace.calendarColorOverrides)
            {
                overrideValue.calendarId = overrideValue.calendarId.trimmed();
                overrideValue.color = overrideValue.color.trimmed();
                if (overrideValue.calendarId.isEmpty() || overrideValue.color.isEmpty())
                    return invalidValue(settingKey(workspaceCalendarColorsKey),
                                        QStringLiteral("calendar color entry is incomplete"));
            }
            std::ranges::sort(snapshot.workspace.calendarColorOverrides, {},
                              &javelin::protocol::CalendarColorOverride::calendarId);
            const auto duplicate =
                std::ranges::adjacent_find(snapshot.workspace.calendarColorOverrides, {},
                                           &javelin::protocol::CalendarColorOverride::calendarId);
            if (duplicate != snapshot.workspace.calendarColorOverrides.end())
                return invalidValue(settingKey(workspaceCalendarColorsKey),
                                    QStringLiteral("calendar color ids must be unique"));
            return std::nullopt;
        }
    } // namespace

    SettingsRepository::SettingsRepository() : SettingsRepository(canonicalSettings())
    {
    }

    SettingsRepository::SettingsRepository(std::unique_ptr<QSettings> settings,
                                           AccountCredentialStore* credentialStore)
        : m_settings(std::move(settings)), m_credentialStore(credentialStore)
    {
    }

    std::unique_ptr<QSettings> SettingsRepository::canonicalSettings()
    {
        return std::make_unique<QSettings>(QSettings::NativeFormat, QSettings::UserScope,
                                           QStringLiteral("Javelin Mail"),
                                           QStringLiteral("Javelin Mail"));
    }

    SettingsReadResult SettingsRepository::load()
    {
        if (const auto error = migrateIfNeeded())
            return *error;
        return readSnapshot();
    }

    javelin::protocol::SettingsUpdateReply
    SettingsRepository::update(javelin::protocol::UpdateSettingsRequest request)
    {
        const auto validation =
            javelin::protocol::validate(javelin::protocol::ClientRequest{request});
        if (validation.has_value())
        {
            return javelin::protocol::SettingsUpdateRejected{
                .currentRevision = request.baseRevision, .error = *validation};
        }

        const auto currentResult = load();
        if (const auto* error = std::get_if<SettingsRepositoryError>(&currentResult))
        {
            return javelin::protocol::SettingsUpdateRejected{
                .currentRevision = request.baseRevision,
                .error = {.code = javelin::protocol::BoundaryErrorCode::SettingsStorageFailure,
                          .field = error->key,
                          .detail = error->detail}};
        }
        auto snapshot = std::get<javelin::protocol::SettingsSnapshot>(currentResult);
        if (snapshot.revision != request.baseRevision)
        {
            return javelin::protocol::SettingsUpdateRejected{
                .currentRevision = snapshot.revision,
                .error = {.code = javelin::protocol::BoundaryErrorCode::StaleSettingsRevision,
                          .field = QStringLiteral("settings.revision"),
                          .detail = QStringLiteral("settings changed since this update was read")}};
        }

        if (const auto error = applyUpdate(snapshot, request.update))
        {
            return javelin::protocol::SettingsUpdateRejected{
                .currentRevision = snapshot.revision,
                .error = {.code = javelin::protocol::BoundaryErrorCode::InvalidRequest,
                          .field = error->key,
                          .detail = error->detail}};
        }
        ++snapshot.revision.value;
        if (const auto error = writeSnapshot(snapshot))
        {
            return javelin::protocol::SettingsUpdateRejected{
                .currentRevision = request.baseRevision,
                .error = {.code = javelin::protocol::BoundaryErrorCode::SettingsStorageFailure,
                          .field = error->key,
                          .detail = error->detail}};
        }
        return javelin::protocol::SettingsUpdated{.revision = snapshot.revision};
    }

    std::optional<SettingsRepositoryError> SettingsRepository::migrateIfNeeded()
    {
        auto& settings = *m_settings;
        settings.sync();
        if (const auto error = settingsStatusError(
                settings, SettingsRepositoryErrorCode::ReadFailed, settingKey(schemaVersionKey)))
            return error;
        const bool hasSchema = settings.contains(settingKey(schemaVersionKey));
        unsigned int storedSchemaVersion = 0;
        if (hasSchema)
        {
            bool ok = false;
            const auto version = settings.value(settingKey(schemaVersionKey)).toUInt(&ok);
            if (!ok || (version != 1 && version != 2 && version != 3 && version != 4 &&
                        version != 5 && version != settingsSchemaVersion))
            {
                return SettingsRepositoryError{
                    .code = SettingsRepositoryErrorCode::UnsupportedSchema,
                    .key = settingKey(schemaVersionKey),
                    .detail = QStringLiteral("unsupported settings schema")};
            }
            if (version == settingsSchemaVersion)
                return std::nullopt;
            storedSchemaVersion = version;
        }

        if (const auto error = migrateLegacyCredentials())
            return error;

        const auto legacy = readSnapshot(storedSchemaVersion < 5);
        if (const auto* error = std::get_if<SettingsRepositoryError>(&legacy))
        {
            return SettingsRepositoryError{.code = SettingsRepositoryErrorCode::MigrationFailed,
                                           .key = error->key,
                                           .detail = error->detail};
        }
        auto migrated = std::get<javelin::protocol::SettingsSnapshot>(legacy);
        if (storedSchemaVersion == 4)
        {
            for (auto& account : migrated.accounts)
            {
                if (account.reauthenticationRequired && account.oauthResource.isEmpty() &&
                    !account.tokenEndpoint.isEmpty() && !account.oauthClientId.isEmpty())
                {
                    account.reauthenticationRequired = false;
                }
            }
        }
        migrated.schemaVersion = settingsSchemaVersion;
        migrated.revision = {};
        if (const auto error = writeSnapshot(migrated, false))
        {
            return SettingsRepositoryError{.code = SettingsRepositoryErrorCode::MigrationFailed,
                                           .key = error->key,
                                           .detail = error->detail};
        }
        const auto verified = readSnapshot();
        const auto* verifiedSnapshot = std::get_if<javelin::protocol::SettingsSnapshot>(&verified);
        if (verifiedSnapshot == nullptr || *verifiedSnapshot != migrated)
        {
            const auto* error = std::get_if<SettingsRepositoryError>(&verified);
            return SettingsRepositoryError{
                .code = SettingsRepositoryErrorCode::MigrationFailed,
                .key = error == nullptr ? settingKey(schemaVersionKey) : error->key,
                .detail = error == nullptr
                              ? QStringLiteral("settings migration verification failed")
                              : error->detail};
        }

        settings.setValue(settingKey(schemaVersionKey), settingsSchemaVersion);
        settings.sync();
        if (const auto error =
                settingsStatusError(settings, SettingsRepositoryErrorCode::MigrationFailed,
                                    settingKey(schemaVersionKey)))
            return error;
        if (settings.value(settingKey(schemaVersionKey)).toUInt() != settingsSchemaVersion)
        {
            return SettingsRepositoryError{
                .code = SettingsRepositoryErrorCode::MigrationFailed,
                .key = settingKey(schemaVersionKey),
                .detail = QStringLiteral("settings schema verification failed")};
        }

        settings.remove(settingKey(legacyWindowGroup));
        settings.remove(settingKey(legacyCalendarColorsKey));
        settings.sync();
        if (const auto error =
                settingsStatusError(settings, SettingsRepositoryErrorCode::MigrationFailed,
                                    settingKey(schemaVersionKey)))
            return error;
        return std::nullopt;
    }

    std::optional<SettingsRepositoryError> SettingsRepository::migrateLegacyCredentials()
    {
        auto& settings = *m_settings;
        settings.beginGroup(settingKey(accountsGroup));
        const int accountCount = settings.beginReadArray(settingKey(accountsSizeKey));
        if (accountCount < 0 || accountCount > maximumAccounts)
        {
            settings.endArray();
            settings.endGroup();
            return SettingsRepositoryError{
                .code = SettingsRepositoryErrorCode::MigrationFailed,
                .key = settingKey(accountsSizeKey),
                .detail = QStringLiteral("invalid account count during credential migration")};
        }

        for (int index = 0; index < accountCount; ++index)
        {
            settings.setArrayIndex(index);
            const auto connectionId = settings.value(settingKey(accountIdKey)).toString().trimmed();
            const AccountCredentialSecrets credentials{
                .accessToken = settings.value(settingKey(accountApiKeyKey)).toString().trimmed(),
                .refreshToken =
                    settings.value(settingKey(accountRefreshTokenKey)).toString().trimmed(),
                .registrationAccessToken =
                    settings.value(settingKey(accountRegistrationAccessTokenKey))
                        .toString()
                        .trimmed(),
            };
            if (credentials.empty())
                continue;
            if (connectionId.isEmpty())
            {
                settings.endArray();
                settings.endGroup();
                return SettingsRepositoryError{
                    .code = SettingsRepositoryErrorCode::MigrationFailed,
                    .key = settingKey(accountIdKey),
                    .detail = QStringLiteral("account id is required for credential migration")};
            }
            if (m_credentialStore == nullptr)
            {
                settings.endArray();
                settings.endGroup();
                return SettingsRepositoryError{
                    .code = SettingsRepositoryErrorCode::MigrationFailed,
                    .key = settingKey(accountApiKeyKey),
                    .detail = QStringLiteral("secure credential storage is unavailable")};
            }
            if (const auto error = m_credentialStore->store(connectionId, credentials))
            {
                settings.endArray();
                settings.endGroup();
                return SettingsRepositoryError{.code = SettingsRepositoryErrorCode::MigrationFailed,
                                               .key = settingKey(accountApiKeyKey),
                                               .detail = error->detail};
            }
        }
        settings.endArray();
        settings.endGroup();
        return std::nullopt;
    }

    SettingsReadResult SettingsRepository::readSnapshot(const bool includeLegacyWorkspace)
    {
        auto& settings = *m_settings;
        javelin::protocol::SettingsSnapshot snapshot;
        bool ok = false;
        const auto storedRevision = settings.value(settingKey(revisionKey), 0).toULongLong(&ok);
        if (!ok)
            return invalidValue(settingKey(revisionKey), QStringLiteral("revision is not numeric"));
        snapshot.revision = {.value = storedRevision};

        settings.beginGroup(settingKey(accountsGroup));
        const int accountCount = settings.beginReadArray(settingKey(accountsSizeKey));
        if (accountCount < 0 || accountCount > maximumAccounts)
        {
            settings.endArray();
            settings.endGroup();
            return invalidValue(settingKey(accountsSizeKey),
                                QStringLiteral("invalid account count"));
        }
        snapshot.accounts.reserve(static_cast<std::size_t>(accountCount));
        for (int index = 0; index < accountCount; ++index)
        {
            settings.setArrayIndex(index);
            const auto id = settings.value(settingKey(accountIdKey)).toString().trimmed();
            if (id.isEmpty())
            {
                settings.endArray();
                settings.endGroup();
                return invalidValue(settingKey(accountIdKey),
                                    QStringLiteral("account id is required"));
            }
            const auto accountRevision =
                settings.value(settingKey(accountRevisionKey), 0).toULongLong(&ok);
            if (!ok)
            {
                settings.endArray();
                settings.endGroup();
                return invalidValue(settingKey(accountRevisionKey),
                                    QStringLiteral("account revision is not numeric"));
            }
            const auto tokenExpiresAtEpochSeconds =
                settings.value(settingKey(accountTokenExpiresAtKey), 0).toLongLong(&ok);
            if (!ok)
            {
                settings.endArray();
                settings.endGroup();
                return invalidValue(settingKey(accountTokenExpiresAtKey),
                                    QStringLiteral("token expiry is not numeric"));
            }
            snapshot.accounts.push_back({
                .id = id,
                .revision = accountRevision,
                .displayName =
                    settings.value(settingKey(accountDisplayNameKey)).toString().trimmed(),
                .sessionUrl = settings.value(settingKey(accountSessionUrlKey)).toString().trimmed(),
                .loginEmail = settings.value(settingKey(accountLoginEmailKey)).toString().trimmed(),
                .tokenEndpoint =
                    settings.value(settingKey(accountTokenEndpointKey)).toString().trimmed(),
                .oauthClientId =
                    settings.value(settingKey(accountOauthClientIdKey)).toString().trimmed(),
                .oauthIssuer =
                    settings.value(settingKey(accountOauthIssuerKey)).toString().trimmed(),
                .oauthResource =
                    settings.value(settingKey(accountOauthResourceKey)).toString().trimmed(),
                .oauthScope =
                    settings.value(settingKey(accountOauthScopeKey)).toString().simplified(),
                .revocationEndpoint =
                    settings.value(settingKey(accountRevocationEndpointKey)).toString().trimmed(),
                .registrationClientUri = settings.value(settingKey(accountRegistrationClientUriKey))
                                             .toString()
                                             .trimmed(),
                .hasCredentials = false,
                .credentialHandle = {},
                .tokenExpiresAtEpochSeconds = tokenExpiresAtEpochSeconds,
                .reauthenticationRequired =
                    settings.value(settingKey(accountReauthenticationRequiredKey), false).toBool(),
                .cachedAccountIds =
                    toVector(settings.value(settingKey(accountCachedIdsKey)).toStringList()),
            });
        }
        settings.endArray();
        settings.endGroup();

        if (const auto error = readSelections(settings, settingKey(syncedMailboxGroup),
                                              snapshot.syncedMailboxSelections))
            return *error;
        if (const auto error = readSelections(settings, settingKey(notificationMailboxGroup),
                                              snapshot.notificationMailboxSelections))
            return *error;

        snapshot.remoteContentSenders =
            toVector(settings
                         .value(settingKey(remoteContentGroup) + QLatin1Char('/') +
                                settingKey(remoteContentSendersKey))
                         .toStringList());
        snapshot.remoteContentDomains =
            toVector(settings
                         .value(settingKey(remoteContentGroup) + QLatin1Char('/') +
                                settingKey(remoteContentDomainsKey))
                         .toStringList());
        snapshot.appearance.messageColorMode =
            settings
                .value(settingKey(appearanceGroup) + QLatin1Char('/') +
                           settingKey(appearanceColorModeKey),
                       0)
                .toInt(&ok);
        if (!ok || snapshot.appearance.messageColorMode < 0 ||
            snapshot.appearance.messageColorMode > 2)
            return invalidValue(
                settingKey(appearanceColorModeKey),
                QStringLiteral("message color mode is outside the supported range"));

        snapshot.attachments = {
            .alwaysAsk = settings
                             .value(settingKey(attachmentsGroup) + QLatin1Char('/') +
                                        settingKey(attachmentsAlwaysAskKey),
                                    true)
                             .toBool(),
            .directory = settings
                             .value(settingKey(attachmentsGroup) + QLatin1Char('/') +
                                    settingKey(attachmentsDirectoryKey))
                             .toString(),
        };
        snapshot.undoSendDelaySeconds =
            settings.value(settingKey(composeUndoSendDelayKey), 10).toInt(&ok);
        if (!ok || snapshot.undoSendDelaySeconds < 1 || snapshot.undoSendDelaySeconds > 120)
            return invalidValue(settingKey(composeUndoSendDelayKey),
                                QStringLiteral("undo-send delay is outside the supported range"));
        if (const auto error = readWorkspace(settings, snapshot.workspace, includeLegacyWorkspace))
            return *error;
        return snapshot;
    }

    std::optional<SettingsRepositoryError>
    SettingsRepository::writeSnapshot(const javelin::protocol::SettingsSnapshot& snapshot,
                                      const bool includeSchemaVersion)
    {
        auto& settings = *m_settings;
        settings.beginGroup(settingKey(accountsGroup));
        settings.beginWriteArray(settingKey(accountsSizeKey),
                                 static_cast<int>(snapshot.accounts.size()));
        for (int index = 0; index < static_cast<int>(snapshot.accounts.size()); ++index)
        {
            settings.setArrayIndex(index);
            const auto& account = snapshot.accounts[static_cast<std::size_t>(index)];
            settings.setValue(settingKey(accountIdKey), account.id);
            settings.setValue(settingKey(accountRevisionKey),
                              static_cast<qulonglong>(account.revision));
            settings.setValue(settingKey(accountDisplayNameKey), account.displayName);
            settings.setValue(settingKey(accountSessionUrlKey), account.sessionUrl);
            settings.setValue(settingKey(accountLoginEmailKey), account.loginEmail);
            settings.remove(settingKey(accountApiKeyKey));
            settings.remove(settingKey(accountRefreshTokenKey));
            settings.setValue(settingKey(accountTokenEndpointKey), account.tokenEndpoint);
            settings.setValue(settingKey(accountOauthClientIdKey), account.oauthClientId);
            settings.setValue(settingKey(accountOauthIssuerKey), account.oauthIssuer);
            settings.setValue(settingKey(accountOauthResourceKey), account.oauthResource);
            settings.setValue(settingKey(accountOauthScopeKey), account.oauthScope);
            settings.setValue(settingKey(accountRevocationEndpointKey), account.revocationEndpoint);
            settings.setValue(settingKey(accountRegistrationClientUriKey),
                              account.registrationClientUri);
            settings.remove(settingKey(accountRegistrationAccessTokenKey));
            settings.setValue(settingKey(accountTokenExpiresAtKey),
                              account.tokenExpiresAtEpochSeconds);
            settings.setValue(settingKey(accountReauthenticationRequiredKey),
                              account.reauthenticationRequired);
            settings.setValue(settingKey(accountCachedIdsKey),
                              toStringList(account.cachedAccountIds));
        }
        settings.endArray();
        settings.endGroup();

        writeSelections(settings, settingKey(syncedMailboxGroup), snapshot.syncedMailboxSelections);
        writeSelections(settings, settingKey(notificationMailboxGroup),
                        snapshot.notificationMailboxSelections);
        settings.setValue(settingKey(remoteContentGroup) + QLatin1Char('/') +
                              settingKey(remoteContentSendersKey),
                          toStringList(snapshot.remoteContentSenders));
        settings.setValue(settingKey(remoteContentGroup) + QLatin1Char('/') +
                              settingKey(remoteContentDomainsKey),
                          toStringList(snapshot.remoteContentDomains));
        settings.setValue(settingKey(appearanceGroup) + QLatin1Char('/') +
                              settingKey(appearanceColorModeKey),
                          snapshot.appearance.messageColorMode);
        settings.setValue(settingKey(attachmentsGroup) + QLatin1Char('/') +
                              settingKey(attachmentsAlwaysAskKey),
                          snapshot.attachments.alwaysAsk);
        settings.setValue(settingKey(attachmentsGroup) + QLatin1Char('/') +
                              settingKey(attachmentsDirectoryKey),
                          snapshot.attachments.directory);
        settings.setValue(settingKey(composeUndoSendDelayKey), snapshot.undoSendDelaySeconds);
        settings.beginGroup(settingKey(workspaceGroup));
        settings.setValue(settingKey(workspaceFormatVersionKey), snapshot.workspace.formatVersion);
        settings.setValue(settingKey(workspaceWindowStateKey), snapshot.workspace.mainWindowState);
        settings.setValue(settingKey(workspaceComposeRichTextDefaultKey),
                          snapshot.workspace.composeRichTextDefault);
        settings.setValue(settingKey(workspaceEmailContextMenuLayoutKey),
                          toStringList(snapshot.workspace.emailContextMenuLayout));
        settings.beginWriteArray(
            settingKey(workspaceCalendarColorsKey),
            static_cast<int>(snapshot.workspace.calendarColorOverrides.size()));
        for (int index = 0;
             index < static_cast<int>(snapshot.workspace.calendarColorOverrides.size()); ++index)
        {
            settings.setArrayIndex(index);
            const auto& overrideValue =
                snapshot.workspace.calendarColorOverrides[static_cast<std::size_t>(index)];
            settings.setValue(settingKey(workspaceCalendarIdKey), overrideValue.calendarId);
            settings.setValue(settingKey(workspaceColorKey), overrideValue.color);
        }
        settings.endArray();
        settings.endGroup();
        if (includeSchemaVersion)
            settings.setValue(settingKey(schemaVersionKey), snapshot.schemaVersion);
        settings.setValue(settingKey(revisionKey),
                          static_cast<qulonglong>(snapshot.revision.value));
        settings.sync();
        if (const auto error = settingsStatusError(
                settings, SettingsRepositoryErrorCode::WriteFailed, settingKey(revisionKey)))
            return error;

        const auto storedRevision = settings.value(settingKey(revisionKey)).toULongLong();
        if (storedRevision != snapshot.revision.value)
        {
            return SettingsRepositoryError{
                .code = SettingsRepositoryErrorCode::WriteFailed,
                .key = settingKey(revisionKey),
                .detail = QStringLiteral("settings revision verification failed")};
        }
        return std::nullopt;
    }

} // namespace javelin::app
