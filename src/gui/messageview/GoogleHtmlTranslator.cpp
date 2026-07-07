#include "gui/messageview/GoogleHtmlTranslator.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>

#include <memory>

namespace javelin::gui::messageview
{
    namespace
    {
        constexpr qsizetype maximumBatchCharacters = 800;

        [[nodiscard]] QString googleApiKey()
        {
            static constexpr std::array<unsigned char, 39> bytes = {
                65,  73,  122, 97, 83,  121, 65, 84, 66,  88, 97,  106, 118,
                122, 81,  76,  84, 68,  72,  69, 81, 98,  99, 112, 113, 48,
                73,  104, 101, 48, 118, 87,  68, 72, 109, 79, 53,  50,  48};
            return QString::fromLatin1(reinterpret_cast<const char*>(bytes.data()),
                                       static_cast<qsizetype>(bytes.size()));
        }

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

        [[nodiscard]] QNetworkRequest makeRequest()
        {
            QNetworkRequest request{
                QUrl{QStringLiteral("https://translate-pa.googleapis.com/v1/translateHtml")}};
            request.setHeader(QNetworkRequest::ContentTypeHeader,
                              QStringLiteral("application/application/json+protobuf"));
            request.setRawHeader("X-goog-api-key", googleApiKey().toLatin1());
            return request;
        }

        [[nodiscard]] QByteArray
        requestBody(const QVector<GoogleHtmlTranslator::PendingRequest>& requests,
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
    } // namespace

    GoogleHtmlTranslator::GoogleHtmlTranslator(QObject* parent) : QObject(parent)
    {
    }

    void GoogleHtmlTranslator::translate(TranslationChunks sourceChunks, QString sourceLanguage,
                                         QString targetLanguage, Callback callback)
    {
        if (sourceChunks.empty())
        {
            callback(TranslationChunks{});
            return;
        }

        auto batches = makeRequestBatches(sourceChunks);
        auto state = std::make_shared<RunState>();
        state->results.resize(sourceChunks.size());
        state->remainingBatches = batches.size();
        state->callback = std::move(callback);

        for (auto& batch : batches)
        {
            translateBatch(std::move(batch), sourceLanguage, targetLanguage, state);
        }
    }

    QVector<QVector<GoogleHtmlTranslator::PendingRequest>>
    GoogleHtmlTranslator::makeRequestBatches(const TranslationChunks& sourceChunks)
    {
        QVector<QVector<PendingRequest>> batches;
        QVector<PendingRequest> currentBatch;
        qsizetype currentSize = 0;

        for (qsizetype i = 0; i < sourceChunks.size(); ++i)
        {
            const auto requestText =
                transformRequest(sourceChunks[i]).replace(QChar{0x200b}, QLatin1Char(' '));
            currentBatch.push_back({
                .chunkIndex = i,
                .requestText = requestText,
            });
            currentSize += requestText.size();
            if (currentSize > maximumBatchCharacters)
            {
                batches.push_back(std::move(currentBatch));
                currentBatch = {};
                currentSize = 0;
            }
        }

        if (!currentBatch.empty())
        {
            batches.push_back(std::move(currentBatch));
        }
        return batches;
    }

    void GoogleHtmlTranslator::translateBatch(QVector<PendingRequest> requests,
                                              QString sourceLanguage, QString targetLanguage,
                                              std::shared_ptr<RunState> state)
    {
        auto* manager = new QNetworkAccessManager(this);
        auto* reply =
            manager->post(makeRequest(), requestBody(requests, sourceLanguage, targetLanguage));
        connect(reply, &QNetworkReply::finished, this,
                [this, manager, reply, requests = std::move(requests),
                 state = std::move(state)]() mutable
                {
                    const auto cleanup = [manager, reply]
                    {
                        reply->deleteLater();
                        manager->deleteLater();
                    };

                    if (reply->error() != QNetworkReply::NoError)
                    {
                        const auto message = reply->errorString();
                        cleanup();
                        if (!state->finished)
                        {
                            state->finished = true;
                            state->callback(message);
                        }
                        return;
                    }

                    const auto parsed = parseResponse(reply->readAll());
                    cleanup();
                    if (const auto* error = std::get_if<QString>(&parsed))
                    {
                        if (!state->finished)
                        {
                            state->finished = true;
                            state->callback(*error);
                        }
                        return;
                    }

                    if (state->finished)
                    {
                        return;
                    }

                    const auto& translations = std::get<QVector<QString>>(parsed);
                    for (qsizetype i = 0; i < requests.size() && i < translations.size(); ++i)
                    {
                        state->results[requests[i].chunkIndex] = transformResponse(translations[i]);
                    }

                    --state->remainingBatches;
                    if (state->remainingBatches == 0)
                    {
                        state->finished = true;
                        state->callback(std::move(state->results));
                    }
                });
    }

    QString GoogleHtmlTranslator::transformRequest(const QStringList& sourceArray)
    {
        QStringList escaped;
        escaped.reserve(sourceArray.size());
        if (sourceArray.size() > 1)
        {
            for (qsizetype i = 0; i < sourceArray.size(); ++i)
            {
                escaped.push_back(
                    QStringLiteral("<a i=%1>%2</a>").arg(i).arg(escapeHtml(sourceArray[i])));
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

    QStringList GoogleHtmlTranslator::transformResponse(QString result)
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
                taggedText = match.captured(1) + outsideText + match.captured(3);
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
        for (qsizetype i = 0; i < indexes.size() && i < resultArray.size(); ++i)
        {
            const auto resultIndex = indexes[i];
            if (output.size() <= resultIndex)
            {
                output.resize(resultIndex + 1);
            }
            if (!output[resultIndex].isEmpty())
            {
                output[resultIndex] += QLatin1Char(' ');
            }
            output[resultIndex] += resultArray[i];
        }

        return output;
    }

} // namespace javelin::gui::messageview
