#include "gui/translation/GoogleTranslationBackend.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif
#include <QCoroNetworkReply>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <KLocalizedString>

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QUrl>

#include <array>
#include <variant>

namespace javelin::gui::translation
{
    namespace
    {
        constexpr qsizetype maximumBatchCharacters = 800;
        constexpr int requestTimeoutMs = 30000;
        constexpr auto backendRevision = "google-translate-html-v1";

        struct PendingText
        {
            qsizetype index = 0;
            QString requestText;
        };

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

        [[nodiscard]] QString transformRequest(const QString& source)
        {
            return QStringLiteral("<pre>%1</pre>").arg(escapeHtml(source));
        }

        [[nodiscard]] QString transformResponse(QString result)
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
                sentences.push_back(
                    result.sliced(sentenceStart + 3, sentenceEnd - sentenceStart - 3));
                index = sentenceEnd;
            }
            if (!sentences.empty())
            {
                result = sentences.join(QLatin1Char(' '));
            }
            result.replace(QStringLiteral("</b>"), QString{});
            result.replace(QRegularExpression{QStringLiteral("</?a(?:\\s[^>]*)?>")}, QString{});
            return result;
        }

        [[nodiscard]] QVector<QVector<PendingText>> makeBatches(const QVector<QString>& texts)
        {
            QVector<QVector<PendingText>> batches;
            QVector<PendingText> currentBatch;
            qsizetype currentSize = 0;
            for (qsizetype index = 0; index < texts.size(); ++index)
            {
                const auto requestText =
                    transformRequest(texts[index]).replace(QChar{0x200b}, QLatin1Char(' '));
                if (!currentBatch.empty() &&
                    currentSize + requestText.size() > maximumBatchCharacters)
                {
                    batches.push_back(std::move(currentBatch));
                    currentBatch = {};
                    currentSize = 0;
                }
                currentBatch.push_back({.index = index, .requestText = requestText});
                currentSize += requestText.size();
            }
            if (!currentBatch.empty())
            {
                batches.push_back(std::move(currentBatch));
            }
            return batches;
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

        [[nodiscard]] QByteArray requestBody(const QVector<PendingText>& requests,
                                             const QString& sourceLanguage,
                                             const QString& targetLanguage)
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

        [[nodiscard]] std::variant<QVector<QString>, TranslationError>
        parseResponse(const QByteArray& data)
        {
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(data, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isArray())
            {
                return TranslationError{
                    .code = TranslationErrorCode::GoogleResponseInvalid,
                    .message = i18n("Google Translate returned an invalid response."),
                };
            }
            const auto root = document.array();
            if (root.empty() || !root[0].isArray())
            {
                return TranslationError{
                    .code = TranslationErrorCode::GoogleResponseInvalid,
                    .message =
                        i18n("Google Translate response did not contain translations."),
                };
            }

            QVector<QString> translations;
            const auto translatedArray = root[0].toArray();
            translations.reserve(translatedArray.size());
            for (const auto& item : translatedArray)
            {
                translations.push_back(transformResponse(item.toString()));
            }
            return translations;
        }
    } // namespace

    GoogleTranslationBackend::GoogleTranslationBackend(QNetworkAccessManager& networkAccessManager)
        : m_networkAccessManager(networkAccessManager)
    {
    }

    void GoogleTranslationBackend::setApiKeyOverride(QString apiKeyOverride)
    {
        m_apiKeyOverride = apiKeyOverride.trimmed();
    }

    QString GoogleTranslationBackend::revision(QStringView, QStringView) const
    {
        return QString::fromLatin1(backendRevision);
    }

    QCoro::Task<BackendResult> GoogleTranslationBackend::translate(BackendRequest request)
    {
        if (request.texts.empty())
        {
            co_return BackendTranslation{
                .texts = {},
                .backendRevision = QString::fromLatin1(backendRevision),
            };
        }
        if (request.fetchPolicy != ExternalFetchPolicy::AllowExternalFetch)
        {
            co_return TranslationUnavailable{
                .reason = TranslationUnavailable::Reason::ExternalFetchNotAllowed,
            };
        }

        const auto apiKey = m_apiKeyOverride.isEmpty() ? builtInApiKey() : m_apiKeyOverride;
        const auto sourceLanguage = request.sourceLanguage == QStringLiteral("auto")
                                        ? request.sourceLanguage
                                        : googleLanguageTag(request.sourceLanguage);
        const auto targetLanguage = googleLanguageTag(request.targetLanguage);
        auto batches = makeBatches(request.texts);
        QVector<QString> results;
        results.resize(request.texts.size());
        for (const auto& batch : batches)
        {
            auto* reply = m_networkAccessManager.post(
                makeRequest(apiKey), requestBody(batch, sourceLanguage, targetLanguage));
            co_await qCoro(reply).waitForFinished();
            const auto deleteReply = qScopeGuard([reply]() { reply->deleteLater(); });
            if (reply->error() != QNetworkReply::NoError)
            {
                co_return TranslationError{
                    .code = TranslationErrorCode::GoogleRequestFailed,
                    .message = reply->errorString(),
                };
            }

            const auto parsed = parseResponse(reply->readAll());
            if (const auto* error = std::get_if<TranslationError>(&parsed))
            {
                co_return *error;
            }
            const auto& translations = std::get<QVector<QString>>(parsed);
            if (translations.size() != batch.size())
            {
                co_return TranslationError{
                    .code = TranslationErrorCode::GoogleResponseInvalid,
                    .message = i18n("Google Translate returned an incomplete response."),
                };
            }
            for (qsizetype index = 0; index < batch.size(); ++index)
            {
                results[batch[index].index] = translations[index];
            }
        }

        co_return BackendTranslation{
            .texts = std::move(results),
            .backendRevision = QString::fromLatin1(backendRevision),
        };
    }

    QString GoogleTranslationBackend::builtInApiKey()
    {
        static constexpr std::array<unsigned char, 39> bytes = {
            65,  73,  122, 97, 83,  121, 65, 84, 66,  88, 97,  106, 118,
            122, 81,  76,  84, 68,  72,  69, 81, 98,  99, 112, 113, 48,
            73,  104, 101, 48, 118, 87,  68, 72, 109, 79, 53,  50,  48};
        return QString::fromLatin1(reinterpret_cast<const char*>(bytes.data()),
                                   static_cast<qsizetype>(bytes.size()));
    }
} // namespace javelin::gui::translation
