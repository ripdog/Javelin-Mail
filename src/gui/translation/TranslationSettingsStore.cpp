#include "gui/translation/TranslationSettingsStore.h"

#include <QSettings>

#include <utility>

namespace javelin::gui::translation
{
    namespace
    {
        constexpr auto translationGroup = "translation";
        constexpr auto providerKey = "provider";
        constexpr auto legacyEnabledKey = "enabled";
        constexpr auto apiKeyOverrideKey = "apiKeyOverride";
        constexpr auto targetLanguageKey = "targetLanguage";
        constexpr auto autoTranslateSendersKey = "autoTranslateSenders";
        constexpr auto autoTranslateDomainsKey = "autoTranslateDomains";
        constexpr auto schemaVersionKey = "schemaVersion";
        constexpr int currentSchemaVersion = 1;

        [[nodiscard]] TranslationError settingsError(const TranslationErrorCode code,
                                                     const QSettings::Status status,
                                                     const QString& action)
        {
            return {
                .code = code,
                .message =
                    QStringLiteral("Could not %1 translation preferences (QSettings status %2).")
                        .arg(action)
                        .arg(static_cast<int>(status)),
            };
        }
    } // namespace

    TranslationSettingsResult TranslationSettingsStore::load() const
    {
        QSettings settings;
        settings.beginGroup(QLatin1StringView{translationGroup});
        const bool hasProvider = settings.contains(QLatin1StringView{providerKey});
        settings.endGroup();
        if (!hasProvider)
        {
            return migrateLegacySettings();
        }

        settings.beginGroup(QLatin1StringView{translationGroup});
        const auto provider = translationProviderFromStorage(
            settings.value(QLatin1StringView{providerKey}, QStringLiteral("google")).toString());
        TranslationSettings value{
            .provider = provider.value_or(TranslationProvider::Google),
            .apiKeyOverride = settings.value(QLatin1StringView{apiKeyOverrideKey}).toString(),
            .targetLanguage =
                settings.value(QLatin1StringView{targetLanguageKey}, QStringLiteral("en"))
                    .toString(),
            .autoTranslateSenders =
                settings.value(QLatin1StringView{autoTranslateSendersKey}).toStringList(),
            .autoTranslateDomains =
                settings.value(QLatin1StringView{autoTranslateDomainsKey}).toStringList(),
        };
        settings.endGroup();
        if (settings.status() != QSettings::NoError)
        {
            return settingsError(TranslationErrorCode::SettingsReadFailed, settings.status(),
                                 QStringLiteral("read"));
        }
        return normalizeTranslationSettings(std::move(value));
    }

    std::optional<TranslationError>
    TranslationSettingsStore::save(TranslationSettings settingsValue) const
    {
        settingsValue = normalizeTranslationSettings(std::move(settingsValue));

        QSettings settings;
        settings.beginGroup(QLatin1StringView{translationGroup});
        settings.setValue(QLatin1StringView{providerKey},
                          translationProviderStorageName(settingsValue.provider));
        settings.setValue(QLatin1StringView{apiKeyOverrideKey}, settingsValue.apiKeyOverride);
        settings.setValue(QLatin1StringView{targetLanguageKey}, settingsValue.targetLanguage);
        settings.setValue(QLatin1StringView{autoTranslateSendersKey},
                          settingsValue.autoTranslateSenders);
        settings.setValue(QLatin1StringView{autoTranslateDomainsKey},
                          settingsValue.autoTranslateDomains);
        settings.setValue(QLatin1StringView{schemaVersionKey}, currentSchemaVersion);
        settings.endGroup();
        settings.sync();
        if (settings.status() != QSettings::NoError)
        {
            return settingsError(TranslationErrorCode::SettingsWriteFailed, settings.status(),
                                 QStringLiteral("save"));
        }
        return std::nullopt;
    }

    TranslationSettingsResult TranslationSettingsStore::migrateLegacySettings() const
    {
        QSettings settings;
        settings.beginGroup(QLatin1StringView{translationGroup});
        const bool enabled = settings.value(QLatin1StringView{legacyEnabledKey}, true).toBool();
        TranslationSettings migrated{
            .provider = enabled ? TranslationProvider::Google : TranslationProvider::Disabled,
            .apiKeyOverride = settings.value(QLatin1StringView{apiKeyOverrideKey}).toString(),
            .targetLanguage =
                settings.value(QLatin1StringView{targetLanguageKey}, QStringLiteral("en"))
                    .toString(),
            .autoTranslateSenders =
                settings.value(QLatin1StringView{autoTranslateSendersKey}).toStringList(),
            .autoTranslateDomains =
                settings.value(QLatin1StringView{autoTranslateDomainsKey}).toStringList(),
        };
        settings.endGroup();
        if (settings.status() != QSettings::NoError)
        {
            return settingsError(TranslationErrorCode::SettingsReadFailed, settings.status(),
                                 QStringLiteral("read"));
        }

        migrated = normalizeTranslationSettings(std::move(migrated));
        if (const auto error = save(migrated))
        {
            return *error;
        }

        const auto verified = load();
        if (const auto* error = std::get_if<TranslationError>(&verified))
        {
            return *error;
        }
        if (std::get<TranslationSettings>(verified) != migrated)
        {
            return TranslationError{
                .code = TranslationErrorCode::SettingsWriteFailed,
                .message =
                    QStringLiteral("Translation preference migration could not be verified."),
            };
        }

        settings.beginGroup(QLatin1StringView{translationGroup});
        settings.remove(QLatin1StringView{legacyEnabledKey});
        settings.endGroup();
        settings.sync();
        if (settings.status() != QSettings::NoError)
        {
            return settingsError(TranslationErrorCode::SettingsWriteFailed, settings.status(),
                                 QStringLiteral("save"));
        }
        return migrated;
    }
} // namespace javelin::gui::translation
