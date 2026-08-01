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

        void normalizeList(QStringList& values)
        {
            for (auto& value : values)
                value = value.trimmed().toLower();
            values.removeAll(QString{});
            values.removeDuplicates();
            values.sort(Qt::CaseInsensitive);
        }
    } // namespace

    TranslationSettings normalizeTranslationSettings(TranslationSettings settings)
    {
        settings.apiKeyOverride = settings.apiKeyOverride.trimmed();
        settings.targetLanguage = settings.targetLanguage.trimmed().toLower();
        if (settings.targetLanguage.isEmpty())
            settings.targetLanguage = QStringLiteral("en");
        normalizeList(settings.autoTranslateSenders);
        normalizeList(settings.autoTranslateDomains);
        return settings;
    }

    void setTranslationListValue(QStringList& values, QString value, const bool enabled)
    {
        value = value.trimmed().toLower();
        if (value.isEmpty())
            return;
        values.removeAll(value);
        if (enabled)
            values.push_back(std::move(value));
        values.sort(Qt::CaseInsensitive);
    }

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
