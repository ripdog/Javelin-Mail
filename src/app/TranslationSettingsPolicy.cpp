#include "app/TranslationApplicationPorts.h"

#include <utility>

namespace javelin::app
{
    namespace
    {
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
} // namespace javelin::app
