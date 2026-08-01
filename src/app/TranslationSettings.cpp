#include "app/TranslationApplicationPorts.h"

#include <QSettings>

#include <utility>

namespace javelin::app
{
    namespace
    {
        constexpr auto translationGroup = "translation";
        constexpr auto enabledKey = "enabled";
        constexpr auto apiKeyOverrideKey = "apiKeyOverride";
        constexpr auto targetLanguageKey = "targetLanguage";
        constexpr auto autoTranslateSendersKey = "autoTranslateSenders";
        constexpr auto autoTranslateDomainsKey = "autoTranslateDomains";
    } // namespace

    TranslationSettings loadTranslationSettings()
    {
        QSettings settings;
        settings.beginGroup(QLatin1StringView{translationGroup});
        auto value = normalizeTranslationSettings({
            .enabled = settings.value(QLatin1StringView{enabledKey}, true).toBool(),
            .apiKeyOverride = settings.value(QLatin1StringView{apiKeyOverrideKey}).toString(),
            .targetLanguage =
                settings.value(QLatin1StringView{targetLanguageKey}, QStringLiteral("en"))
                    .toString(),
            .autoTranslateSenders =
                settings.value(QLatin1StringView{autoTranslateSendersKey}).toStringList(),
            .autoTranslateDomains =
                settings.value(QLatin1StringView{autoTranslateDomainsKey}).toStringList(),
        });
        settings.endGroup();
        return value;
    }

    void saveTranslationSettings(TranslationSettings value)
    {
        value = normalizeTranslationSettings(std::move(value));

        QSettings settings;
        settings.beginGroup(QLatin1StringView{translationGroup});
        settings.setValue(QLatin1StringView{enabledKey}, value.enabled);
        settings.setValue(QLatin1StringView{apiKeyOverrideKey}, value.apiKeyOverride);
        settings.setValue(QLatin1StringView{targetLanguageKey}, value.targetLanguage);
        settings.setValue(QLatin1StringView{autoTranslateSendersKey}, value.autoTranslateSenders);
        settings.setValue(QLatin1StringView{autoTranslateDomainsKey}, value.autoTranslateDomains);
        settings.endGroup();
        settings.sync();
    }
} // namespace javelin::app
