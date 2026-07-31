#include "app/SettingsRepository.h"

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
        constexpr auto accountCachedIdsKey = "cachedAccountIds";
        constexpr auto mailboxIdsKey = "mailboxIds";
        constexpr auto syncedMailboxGroup = "mailboxSync";
        constexpr auto notificationMailboxGroup = "mailboxNotifications";
        constexpr auto remoteContentGroup = "remoteContent";
        constexpr auto remoteContentSendersKey = "allowedSenders";
        constexpr auto remoteContentDomainsKey = "allowedDomains";
        constexpr auto translationGroup = "translation";
        constexpr auto translationEnabledKey = "enabled";
        constexpr auto translationApiKeyOverrideKey = "apiKeyOverride";
        constexpr auto translationTargetLanguageKey = "targetLanguage";
        constexpr auto translationSendersKey = "autoTranslateSenders";
        constexpr auto translationDomainsKey = "autoTranslateDomains";
        constexpr auto appearanceGroup = "messageAppearance";
        constexpr auto appearanceColorModeKey = "colorMode";
        constexpr auto attachmentsGroup = "attachments";
        constexpr auto attachmentsAlwaysAskKey = "alwaysAsk";
        constexpr auto attachmentsDirectoryKey = "directory";
        constexpr auto composeUndoSendDelayKey = "compose/undoSendDelaySeconds";
        constexpr int settingsSchemaVersion = 1;
        constexpr int maximumAccounts = 256;
        constexpr int maximumSelections = 256;

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

        void normalizeList(std::vector<QString>& values)
        {
            for (auto& value : values)
                value = value.trimmed().toLower();
            values.erase(std::remove_if(values.begin(), values.end(),
                                        [](const QString& value) { return value.isEmpty(); }),
                         values.end());
            std::ranges::sort(values, [](const QString& left, const QString& right)
                              { return left.compare(right, Qt::CaseInsensitive) < 0; });
            values.erase(std::ranges::unique(values).begin(), values.end());
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
                                  .mailboxIds = toVector(settings.value(key).toStringList()),
                                  .configured = true});
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
                if (!selection.configured)
                    continue;
                settings.setValue(groupKey(selection.accountId),
                                  toStringList(selection.mailboxIds));
            }
            settings.endGroup();
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
            if (update.translation.has_value())
                snapshot.translation = *update.translation;
            if (update.appearance.has_value())
                snapshot.appearance = *update.appearance;
            if (update.attachments.has_value())
                snapshot.attachments = *update.attachments;
            if (update.undoSendDelaySeconds.has_value())
                snapshot.undoSendDelaySeconds = *update.undoSendDelaySeconds;

            normalizeList(snapshot.translation.autoTranslateSenders);
            normalizeList(snapshot.translation.autoTranslateDomains);
            snapshot.translation.apiKeyOverride = snapshot.translation.apiKeyOverride.trimmed();
            snapshot.translation.targetLanguage =
                snapshot.translation.targetLanguage.trimmed().toLower();
            if (snapshot.translation.targetLanguage.isEmpty())
                snapshot.translation.targetLanguage = QStringLiteral("en");
            return std::nullopt;
        }
    } // namespace

    SettingsRepository::SettingsRepository() : SettingsRepository(canonicalSettings())
    {
    }

    SettingsRepository::SettingsRepository(std::unique_ptr<QSettings> settings)
        : m_settings(std::move(settings))
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
        if (settings.contains(settingKey(schemaVersionKey)))
        {
            bool ok = false;
            const auto version = settings.value(settingKey(schemaVersionKey)).toUInt(&ok);
            if (!ok || version != settingsSchemaVersion)
            {
                return SettingsRepositoryError{
                    .code = SettingsRepositoryErrorCode::UnsupportedSchema,
                    .key = settingKey(schemaVersionKey),
                    .detail = QStringLiteral("unsupported settings schema")};
            }
            return std::nullopt;
        }

        const auto legacy = readSnapshot();
        if (const auto* error = std::get_if<SettingsRepositoryError>(&legacy))
        {
            return SettingsRepositoryError{.code = SettingsRepositoryErrorCode::MigrationFailed,
                                           .key = error->key,
                                           .detail = error->detail};
        }
        auto migrated = std::get<javelin::protocol::SettingsSnapshot>(legacy);
        migrated.schemaVersion = settingsSchemaVersion;
        migrated.revision = {};
        if (const auto error = writeSnapshot(migrated))
        {
            return SettingsRepositoryError{.code = SettingsRepositoryErrorCode::MigrationFailed,
                                           .key = error->key,
                                           .detail = error->detail};
        }
        return std::nullopt;
    }

    SettingsReadResult SettingsRepository::readSnapshot()
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
            snapshot.accounts.push_back({
                .id = id,
                .revision = accountRevision,
                .displayName =
                    settings.value(settingKey(accountDisplayNameKey)).toString().trimmed(),
                .sessionUrl = settings.value(settingKey(accountSessionUrlKey)).toString().trimmed(),
                .loginEmail = settings.value(settingKey(accountLoginEmailKey)).toString().trimmed(),
                .apiKey = settings.value(settingKey(accountApiKeyKey)).toString().trimmed(),
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
        snapshot.translation = {
            .enabled = settings
                           .value(settingKey(translationGroup) + QLatin1Char('/') +
                                      settingKey(translationEnabledKey),
                                  true)
                           .toBool(),
            .apiKeyOverride = settings
                                  .value(settingKey(translationGroup) + QLatin1Char('/') +
                                         settingKey(translationApiKeyOverrideKey))
                                  .toString()
                                  .trimmed(),
            .targetLanguage = settings
                                  .value(settingKey(translationGroup) + QLatin1Char('/') +
                                             settingKey(translationTargetLanguageKey),
                                         QStringLiteral("en"))
                                  .toString()
                                  .trimmed()
                                  .toLower(),
            .autoTranslateSenders =
                toVector(settings
                             .value(settingKey(translationGroup) + QLatin1Char('/') +
                                    settingKey(translationSendersKey))
                             .toStringList()),
            .autoTranslateDomains =
                toVector(settings
                             .value(settingKey(translationGroup) + QLatin1Char('/') +
                                    settingKey(translationDomainsKey))
                             .toStringList()),
        };
        normalizeList(snapshot.translation.autoTranslateSenders);
        normalizeList(snapshot.translation.autoTranslateDomains);
        if (snapshot.translation.targetLanguage.isEmpty())
            snapshot.translation.targetLanguage = QStringLiteral("en");

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
        return snapshot;
    }

    std::optional<SettingsRepositoryError>
    SettingsRepository::writeSnapshot(const javelin::protocol::SettingsSnapshot& snapshot)
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
            settings.setValue(settingKey(accountApiKeyKey), account.apiKey);
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
        settings.setValue(settingKey(translationGroup) + QLatin1Char('/') +
                              settingKey(translationEnabledKey),
                          snapshot.translation.enabled);
        settings.setValue(settingKey(translationGroup) + QLatin1Char('/') +
                              settingKey(translationApiKeyOverrideKey),
                          snapshot.translation.apiKeyOverride);
        settings.setValue(settingKey(translationGroup) + QLatin1Char('/') +
                              settingKey(translationTargetLanguageKey),
                          snapshot.translation.targetLanguage);
        settings.setValue(settingKey(translationGroup) + QLatin1Char('/') +
                              settingKey(translationSendersKey),
                          toStringList(snapshot.translation.autoTranslateSenders));
        settings.setValue(settingKey(translationGroup) + QLatin1Char('/') +
                              settingKey(translationDomainsKey),
                          toStringList(snapshot.translation.autoTranslateDomains));
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
