#include "gui/translation/TranslationTypes.h"

#include <QRegularExpression>

#include <utility>

namespace javelin::gui::translation
{
    namespace
    {
        void normalizeList(QStringList& values)
        {
            for (auto& value : values)
            {
                value = value.trimmed().toLower();
            }
            values.removeAll(QString{});
            values.removeDuplicates();
            values.sort(Qt::CaseInsensitive);
        }

        [[nodiscard]] QString titleCase(QString value)
        {
            if (value.isEmpty())
            {
                return value;
            }
            value = value.toLower();
            value[0] = value[0].toUpper();
            return value;
        }
    } // namespace

    QString canonicalLanguageTag(QString languageTag)
    {
        languageTag = languageTag.trimmed();
        languageTag.replace(QLatin1Char('_'), QLatin1Char('-'));
        if (languageTag.isEmpty())
        {
            return {};
        }

        const auto lower = languageTag.toLower();
        if (lower == QStringLiteral("zh") || lower == QStringLiteral("zh-cn") ||
            lower == QStringLiteral("zh-hans"))
        {
            return QStringLiteral("zh-Hans");
        }
        if (lower == QStringLiteral("zh-tw") || lower == QStringLiteral("zh-hant"))
        {
            return QStringLiteral("zh-Hant");
        }
        if (lower == QStringLiteral("auto"))
        {
            return QStringLiteral("auto");
        }

        auto parts = languageTag.split(QLatin1Char('-'), Qt::SkipEmptyParts);
        if (parts.empty())
        {
            return {};
        }
        parts[0] = parts[0].toLower();
        for (qsizetype index = 1; index < parts.size(); ++index)
        {
            if (parts[index].size() == 4)
            {
                parts[index] = titleCase(parts[index]);
            }
            else if (parts[index].size() == 2 || parts[index].size() == 3)
            {
                parts[index] = parts[index].toUpper();
            }
            else
            {
                parts[index] = parts[index].toLower();
            }
        }
        return parts.join(QLatin1Char('-'));
    }

    QString googleLanguageTag(const QStringView canonicalTag)
    {
        if (canonicalTag == QStringLiteral("zh-Hans"))
        {
            return QStringLiteral("zh-CN");
        }
        if (canonicalTag == QStringLiteral("zh-Hant"))
        {
            return QStringLiteral("zh-TW");
        }
        return canonicalTag.toString();
    }

    QString mozillaLanguageTag(const QStringView canonicalTag)
    {
        if (canonicalTag == QStringLiteral("zh-Hans"))
        {
            return QStringLiteral("zh");
        }
        if (canonicalTag == QStringLiteral("zh-Hant"))
        {
            return QStringLiteral("zh_hant");
        }
        auto value = canonicalTag.toString();
        value.replace(QLatin1Char('-'), QLatin1Char('_'));
        return value;
    }

    QString translationProviderStorageName(const TranslationProvider provider)
    {
        switch (provider)
        {
        case TranslationProvider::Disabled:
            return QStringLiteral("disabled");
        case TranslationProvider::Google:
            return QStringLiteral("google");
        case TranslationProvider::Local:
            return QStringLiteral("local");
        }
        return QStringLiteral("google");
    }

    std::optional<TranslationProvider> translationProviderFromStorage(const QStringView value)
    {
        if (value.compare(QStringLiteral("disabled"), Qt::CaseInsensitive) == 0)
        {
            return TranslationProvider::Disabled;
        }
        if (value.compare(QStringLiteral("google"), Qt::CaseInsensitive) == 0)
        {
            return TranslationProvider::Google;
        }
        if (value.compare(QStringLiteral("local"), Qt::CaseInsensitive) == 0)
        {
            return TranslationProvider::Local;
        }
        return std::nullopt;
    }

    TranslationSettings normalizeTranslationSettings(TranslationSettings settings)
    {
        settings.apiKeyOverride = settings.apiKeyOverride.trimmed();
        settings.targetLanguage = canonicalLanguageTag(std::move(settings.targetLanguage));
        if (settings.targetLanguage.isEmpty() || settings.targetLanguage == QStringLiteral("auto"))
        {
            settings.targetLanguage = QStringLiteral("en");
        }
        normalizeList(settings.autoTranslateSenders);
        normalizeList(settings.autoTranslateDomains);
        return settings;
    }

    void setTranslationListValue(QStringList& values, QString value, const bool enabled)
    {
        value = value.trimmed().toLower();
        if (value.isEmpty())
        {
            return;
        }
        values.removeAll(value);
        if (enabled)
        {
            values.push_back(std::move(value));
        }
        values.sort(Qt::CaseInsensitive);
    }
} // namespace javelin::gui::translation
