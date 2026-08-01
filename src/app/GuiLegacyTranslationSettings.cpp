#include "app/TranslationApplicationPorts.h"

#include <QSettings>

namespace javelin::app
{
    namespace
    {
        void normalize(QStringList& values)
        {
            for (auto& value : values)
                value = value.trimmed().toLower();
            values.removeAll(QString{});
            values.removeDuplicates();
            values.sort(Qt::CaseInsensitive);
        }
    } // namespace

    TranslationSettings loadTranslationSettings()
    {
        QSettings settings;
        settings.beginGroup(QStringLiteral("translation"));
        TranslationSettings value{
            .enabled = settings.value(QStringLiteral("enabled"), true).toBool(),
            .apiKeyOverride = settings.value(QStringLiteral("apiKeyOverride")).toString().trimmed(),
            .targetLanguage = settings.value(QStringLiteral("targetLanguage"), QStringLiteral("en"))
                                  .toString()
                                  .trimmed()
                                  .toLower(),
            .autoTranslateSenders =
                settings.value(QStringLiteral("autoTranslateSenders")).toStringList(),
            .autoTranslateDomains =
                settings.value(QStringLiteral("autoTranslateDomains")).toStringList(),
        };
        settings.endGroup();
        if (value.targetLanguage.isEmpty())
            value.targetLanguage = QStringLiteral("en");
        normalize(value.autoTranslateSenders);
        normalize(value.autoTranslateDomains);
        return value;
    }

    void saveTranslationSettings(TranslationSettings value)
    {
        value.apiKeyOverride = value.apiKeyOverride.trimmed();
        value.targetLanguage = value.targetLanguage.trimmed().toLower();
        if (value.targetLanguage.isEmpty())
            value.targetLanguage = QStringLiteral("en");
        normalize(value.autoTranslateSenders);
        normalize(value.autoTranslateDomains);

        QSettings settings;
        settings.beginGroup(QStringLiteral("translation"));
        settings.setValue(QStringLiteral("enabled"), value.enabled);
        settings.setValue(QStringLiteral("apiKeyOverride"), value.apiKeyOverride);
        settings.setValue(QStringLiteral("targetLanguage"), value.targetLanguage);
        settings.setValue(QStringLiteral("autoTranslateSenders"), value.autoTranslateSenders);
        settings.setValue(QStringLiteral("autoTranslateDomains"), value.autoTranslateDomains);
        settings.endGroup();
        settings.sync();
    }
} // namespace javelin::app
