#include "app/TranslationService.h"

#include "jmap/cache/TranslationCacheRepository.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif
#include <QCoroNetworkReply>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSettings>
#include <QUrl>

#include <algorithm>
#include <array>

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
        constexpr qsizetype maximumBatchCharacters = 800;
        constexpr int requestTimeoutMs = 30000;

        [[nodiscard]] QString escapeHtml(QString text)
        {
            return text.replace(QLatin1String("&"), QStringLiteral("&amp;"))
                .replace(QLatin1String("<"), QStringLiteral("&lt;"))
                .replace(QLatin1String(">"), QStringLiteral("&gt;"))
                .replace(QLatin1String("\""), QStringLiteral("&quot;"))
                .replace(QLatin1String("'"), QStringLiteral("&#39;"));
        }

        [[nodiscard]] QString unescapeHtml(QString text)
        {
            return text.replace(QLatin1String("&amp;"), QStringLiteral("&"))
                .replace(QLatin1String("&lt;"), QStringLiteral("<"))
                .replace(QLatin1String("&gt;"), QStringLiteral(">"))
                .replace(QLatin1String("&quot;"), QStringLiteral("\""))
                .replace(QLatin1String("&#39;"), QStringLiteral("'"));
        }

        [[nodiscard]] QNetworkRequest makeRequest(const QString& apiKey)
        {
            QNetworkRequest request{
                QUrl{QStringLiteral("https://translate-pa.googleapis.com/v1/translateHtml")}};
            request.setTransferTimeout(requestTimeoutMs);
            request.setHeader(QNetworkRequest::ContentTypeHeader,
                              QStringLiteral("application/application/json+protobuf"));
            request.setRawHeader(QByteArrayLiteral("X-goog-api-key"), apiKey.toLatin1());
            return request;
        }

        [[nodiscard]] QByteArray
        requestBody(const QVector<TranslationService::PendingRequest>& requests,
                    const QString& sourceLanguage, const QString& targetLanguage)
        {
            QJsonArray requestTexts;
            for (const auto& request : requests)
            {
                requestTexts.push_back(request.requestText);
            }

            QJsonArray payload;
            payload.push_back(requestTexts);
            payload.push_back(sourceLanguage);
            payload.push_back(targetLanguage);

            QJsonArray body;
            body.push_back(payload);
            body.push_back(QStringLiteral("te"));
            return QJsonDocument{body}.toJson(QJsonDocument::Compact);
        }

        [[nodiscard]] std::variant<QVector<QString>, QString> parseResponse(const QByteArray& data)
        {
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(data, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isArray())
            {
                return QStringLiteral("Google Translate returned an invalid response.");
            }

            const auto root = document.array();
            if (root.empty() || !root[0].isArray())
            {
                return QStringLiteral("Google Translate response did not contain translations.");
            }

            QVector<QString> translations;
            const auto translatedArray = root[0].toArray();
            translations.reserve(translatedArray.size());
            for (const auto& item : translatedArray)
            {
                translations.push_back(item.toString());
            }
            return translations;
        }

        void normalizeSettingsList(QStringList& values)
        {
            for (auto& value : values)
            {
                value = value.trimmed().toLower();
            }
            values.removeAll(QString{});
            values.removeDuplicates();
            values.sort(Qt::CaseInsensitive);
        }
    } // namespace

    TranslationService::TranslationService(
        QNetworkAccessManager& networkAccessManager,
        javelin::jmap::cache::TranslationCacheRepository& translationCacheRepository)
        : m_networkAccessManager(networkAccessManager),
          m_translationCacheRepository(translationCacheRepository), m_settings(loadSettings())
    {
    }

    TranslationSettings TranslationService::loadSettings()
    {
        QSettings settings;
        settings.beginGroup(QLatin1StringView{translationGroup});
        TranslationSettings value{
            .enabled = settings.value(QLatin1StringView{enabledKey}, true).toBool(),
            .apiKeyOverride =
                settings.value(QLatin1StringView{apiKeyOverrideKey}).toString().trimmed(),
            .targetLanguage =
                settings.value(QLatin1StringView{targetLanguageKey}, QStringLiteral("en"))
                    .toString()
                    .trimmed()
                    .toLower(),
            .autoTranslateSenders =
                settings.value(QLatin1StringView{autoTranslateSendersKey}).toStringList(),
            .autoTranslateDomains =
                settings.value(QLatin1StringView{autoTranslateDomainsKey}).toStringList(),
        };
        settings.endGroup();
        if (value.targetLanguage.isEmpty())
        {
            value.targetLanguage = QStringLiteral("en");
        }
        normalizeSettingsList(value.autoTranslateSenders);
        normalizeSettingsList(value.autoTranslateDomains);
        return value;
    }

    void TranslationService::saveSettings(TranslationSettings settingsValue)
    {
        settingsValue.apiKeyOverride = settingsValue.apiKeyOverride.trimmed();
        settingsValue.targetLanguage = settingsValue.targetLanguage.trimmed().toLower();
        if (settingsValue.targetLanguage.isEmpty())
        {
            settingsValue.targetLanguage = QStringLiteral("en");
        }
        normalizeSettingsList(settingsValue.autoTranslateSenders);
        normalizeSettingsList(settingsValue.autoTranslateDomains);

        QSettings settings;
        settings.beginGroup(QLatin1StringView{translationGroup});
        settings.setValue(QLatin1StringView{enabledKey}, settingsValue.enabled);
        settings.setValue(QLatin1StringView{apiKeyOverrideKey}, settingsValue.apiKeyOverride);
        settings.setValue(QLatin1StringView{targetLanguageKey}, settingsValue.targetLanguage);
        settings.setValue(QLatin1StringView{autoTranslateSendersKey},
                          settingsValue.autoTranslateSenders);
        settings.setValue(QLatin1StringView{autoTranslateDomainsKey},
                          settingsValue.autoTranslateDomains);
        settings.endGroup();
        settings.sync();
    }

    void TranslationService::reloadSettings()
    {
        m_settings = loadSettings();
    }

    const TranslationSettings& TranslationService::settings() const
    {
        return m_settings;
    }

    bool TranslationService::isEnabled() const
    {
        return m_settings.enabled;
    }

    QString TranslationService::targetLanguage() const
    {
        return m_settings.targetLanguage;
    }

    bool TranslationService::shouldAutoTranslate(const QString& sender, const QString& domain) const
    {
        return m_settings.enabled &&
               ((!sender.isEmpty() &&
                 m_settings.autoTranslateSenders.contains(sender, Qt::CaseInsensitive)) ||
                (!domain.isEmpty() &&
                 m_settings.autoTranslateDomains.contains(domain, Qt::CaseInsensitive)));
    }

    void TranslationService::setAutoTranslateSender(QString sender, const bool enabled)
    {
        setListValue(m_settings.autoTranslateSenders, std::move(sender), enabled);
        saveSettings(m_settings);
    }

    void TranslationService::setAutoTranslateDomain(QString domain, const bool enabled)
    {
        setListValue(m_settings.autoTranslateDomains, std::move(domain), enabled);
        saveSettings(m_settings);
    }

    QCoro::Task<TranslationService::Result>
    TranslationService::translate(TranslationChunks sourceChunks, QString sourceLanguage,
                                  const bool allowNetwork)
    {
        if (!m_settings.enabled)
        {
            co_return QStringLiteral("Translation is disabled in Preferences.");
        }
        if (sourceChunks.empty())
        {
            co_return TranslationChunks{};
        }

        sourceLanguage = sourceLanguage.trimmed().toLower();
        if (sourceLanguage.isEmpty())
        {
            sourceLanguage = QStringLiteral("auto");
        }
        const auto targetLanguage = m_settings.targetLanguage;

        TranslationChunks cachedChunks;
        cachedChunks.reserve(sourceChunks.size());
        bool cacheComplete = true;
        for (const auto& chunk : sourceChunks)
        {
            QStringList cachedChunk;
            cachedChunk.reserve(chunk.size());
            for (const auto& text : chunk)
            {
                const auto cached =
                    m_translationCacheRepository.find(sourceLanguage, targetLanguage, text);
                if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&cached))
                {
                    qWarning().noquote() << "Translation cache lookup failed" << error->message;
                    cacheComplete = false;
                    break;
                }
                const auto& translated = std::get<std::optional<QString>>(cached);
                if (!translated.has_value())
                {
                    cacheComplete = false;
                    break;
                }
                cachedChunk.push_back(*translated);
            }
            if (!cacheComplete)
            {
                break;
            }
            cachedChunks.push_back(std::move(cachedChunk));
        }
        if (cacheComplete)
        {
            co_return cachedChunks;
        }
        if (!allowNetwork)
        {
            co_return TranslationUnavailable{};
        }

        const auto apiKey =
            m_settings.apiKeyOverride.isEmpty() ? builtInApiKey() : m_settings.apiKeyOverride;
        auto batches = makeRequestBatches(sourceChunks);
        TranslationChunks results;
        results.resize(sourceChunks.size());
        for (const auto& requests : batches)
        {
            auto* reply = m_networkAccessManager.post(
                makeRequest(apiKey), requestBody(requests, sourceLanguage, targetLanguage));
            co_await qCoro(reply).waitForFinished();
            const auto deleteReply = qScopeGuard([reply]() { reply->deleteLater(); });
            if (reply->error() != QNetworkReply::NoError)
            {
                co_return reply->errorString();
            }

            const auto parsed = parseResponse(reply->readAll());
            if (const auto* error = std::get_if<QString>(&parsed))
            {
                co_return *error;
            }
            const auto& translations = std::get<QVector<QString>>(parsed);
            if (translations.size() != requests.size())
            {
                co_return QStringLiteral("Google Translate returned an incomplete response.");
            }
            for (qsizetype index = 0; index < requests.size(); ++index)
            {
                results[requests[index].chunkIndex] = transformResponse(translations[index]);
            }
        }

        for (qsizetype chunkIndex = 0;
             chunkIndex < sourceChunks.size() && chunkIndex < results.size(); ++chunkIndex)
        {
            const auto& sourceChunk = sourceChunks[chunkIndex];
            const auto& translatedChunk = results[chunkIndex];
            for (qsizetype textIndex = 0;
                 textIndex < sourceChunk.size() && textIndex < translatedChunk.size(); ++textIndex)
            {
                if (const auto error = m_translationCacheRepository.upsert({
                        .sourceLanguage = sourceLanguage,
                        .targetLanguage = targetLanguage,
                        .inputText = sourceChunk[textIndex],
                        .translatedText = translatedChunk[textIndex],
                    }))
                {
                    qWarning().noquote() << "Translation cache write failed" << error->message;
                }
            }
        }

        co_return results;
    }

    QString TranslationService::builtInApiKey()
    {
        static constexpr std::array<unsigned char, 39> bytes = {
            65,  73,  122, 97, 83,  121, 65, 84, 66,  88, 97,  106, 118,
            122, 81,  76,  84, 68,  72,  69, 81, 98,  99, 112, 113, 48,
            73,  104, 101, 48, 118, 87,  68, 72, 109, 79, 53,  50,  48};
        return QString::fromLatin1(reinterpret_cast<const char*>(bytes.data()),
                                   static_cast<qsizetype>(bytes.size()));
    }

    QVector<QVector<TranslationService::PendingRequest>>
    TranslationService::makeRequestBatches(const TranslationChunks& sourceChunks)
    {
        QVector<QVector<PendingRequest>> batches;
        QVector<PendingRequest> currentBatch;
        qsizetype currentSize = 0;

        for (qsizetype index = 0; index < sourceChunks.size(); ++index)
        {
            const auto requestText =
                transformRequest(sourceChunks[index]).replace(QChar{0x200b}, QLatin1Char(' '));
            if (!currentBatch.empty() && currentSize + requestText.size() > maximumBatchCharacters)
            {
                batches.push_back(std::move(currentBatch));
                currentBatch = {};
                currentSize = 0;
            }
            currentBatch.push_back({
                .chunkIndex = index,
                .requestText = requestText,
            });
            currentSize += requestText.size();
        }

        if (!currentBatch.empty())
        {
            batches.push_back(std::move(currentBatch));
        }
        return batches;
    }

    QString TranslationService::transformRequest(const QStringList& sourceArray)
    {
        QStringList escaped;
        escaped.reserve(sourceArray.size());
        if (sourceArray.size() > 1)
        {
            for (qsizetype index = 0; index < sourceArray.size(); ++index)
            {
                escaped.push_back(QStringLiteral("<a i=%1>%2</a>")
                                      .arg(index)
                                      .arg(escapeHtml(sourceArray[index])));
            }
        }
        else
        {
            for (const auto& text : sourceArray)
            {
                escaped.push_back(escapeHtml(text));
            }
        }
        return QStringLiteral("<pre>%1</pre>").arg(escaped.join(QString{}));
    }

    QStringList TranslationService::transformResponse(QString result)
    {
        result = unescapeHtml(std::move(result));

        const auto preIndex = result.indexOf(QStringLiteral("<pre"));
        if (preIndex != -1)
        {
            result.replace(QStringLiteral("</pre>"), QString{});
            const auto closeIndex = result.indexOf(QLatin1Char('>'), preIndex);
            if (closeIndex != -1)
            {
                result = result.sliced(closeIndex + 1);
            }
        }

        QStringList sentences;
        qsizetype index = 0;
        while (true)
        {
            const auto sentenceStart = result.indexOf(QStringLiteral("<b>"), index);
            if (sentenceStart == -1)
            {
                break;
            }
            const auto sentenceEnd = result.indexOf(QStringLiteral("<i>"), sentenceStart);
            if (sentenceEnd == -1)
            {
                sentences.push_back(result.sliced(sentenceStart + 3));
                break;
            }

            sentences.push_back(result.sliced(sentenceStart + 3, sentenceEnd - sentenceStart - 3));
            index = sentenceEnd;
        }

        if (!sentences.empty())
        {
            result = sentences.join(QLatin1Char(' '));
        }
        result.replace(QStringLiteral("</b>"), QString{});

        QStringList resultArray;
        qsizetype lastEnd = 0;
        static const QRegularExpression anchorRegex{
            QStringLiteral(R"((\<a\si\=([0-9]+)\>)([^\<\>]*(?=\<\/a\>))*)")};
        auto iterator = anchorRegex.globalMatch(result);
        while (iterator.hasNext())
        {
            const auto match = iterator.next();
            auto taggedText = match.captured(0);
            if (match.capturedStart() > lastEnd)
            {
                const auto outsideText = result.sliced(lastEnd, match.capturedStart() - lastEnd)
                                             .replace(QStringLiteral("</a>"), QString{});
                const auto tagText = match.captured(1);
                taggedText = tagText + outsideText + taggedText.sliced(tagText.size());
            }
            resultArray.push_back(std::move(taggedText));
            lastEnd = match.capturedEnd();
        }

        const auto lastOutsideText =
            result.sliced(lastEnd).replace(QStringLiteral("</a>"), QString{});
        if (!resultArray.empty())
        {
            resultArray.last() += lastOutsideText;
        }

        if (resultArray.empty())
        {
            return {result};
        }

        QVector<qsizetype> indexes;
        indexes.reserve(resultArray.size());
        for (auto& value : resultArray)
        {
            const auto tagEndIndex = value.indexOf(QLatin1Char('>'));
            if (tagEndIndex == -1)
            {
                continue;
            }
            indexes.push_back(value.sliced(0, tagEndIndex)
                                  .remove(QLatin1Char('<'))
                                  .remove(QStringLiteral("a i="))
                                  .toLongLong());
            value = value.sliced(tagEndIndex + 1);
        }

        QStringList output;
        for (qsizetype outputIndex = 0;
             outputIndex < indexes.size() && outputIndex < resultArray.size(); ++outputIndex)
        {
            const auto resultIndex = indexes[outputIndex];
            if (output.size() <= resultIndex)
            {
                output.resize(resultIndex + 1);
            }
            if (!output[resultIndex].isEmpty())
            {
                output[resultIndex] += QLatin1Char(' ');
            }
            output[resultIndex] += resultArray[outputIndex];
        }

        return output;
    }

    QString TranslationService::normalizedListValue(QString value)
    {
        return value.trimmed().toLower();
    }

    void TranslationService::setListValue(QStringList& values, QString value, const bool enabled)
    {
        value = normalizedListValue(std::move(value));
        if (value.isEmpty())
        {
            return;
        }
        values.removeAll(value);
        if (enabled)
        {
            values.push_back(value);
        }
        normalizeSettingsList(values);
    }

} // namespace javelin::app
