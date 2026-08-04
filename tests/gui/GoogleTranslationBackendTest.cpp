#include "gui/translation/GoogleTranslationBackend.h"

#include <QCoroTask>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

namespace
{
    using namespace javelin::gui::translation;

    class ApplicationGuard
    {
      public:
        ApplicationGuard()
        {
            if (QCoreApplication::instance() != nullptr)
            {
                return;
            }
            static int argc = 1;
            static char appName[] = "javelin-google-translation-tests";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    class ScriptedReply final : public QNetworkReply
    {
      public:
        ScriptedReply(QNetworkRequest request, QByteArray payload, QObject* parent)
            : QNetworkReply(parent), m_payload(std::move(payload))
        {
            setRequest(request);
            setUrl(request.url());
            open(QIODevice::ReadOnly | QIODevice::Unbuffered);
            QTimer::singleShot(0, this,
                               [this]
                               {
                                   Q_EMIT readyRead();
                                   setFinished(true);
                                   Q_EMIT finished();
                               });
        }

        void abort() override
        {
            setError(QNetworkReply::OperationCanceledError,
                     QStringLiteral("The scripted request was cancelled."));
            setFinished(true);
        }

        [[nodiscard]] qint64 bytesAvailable() const override
        {
            return (m_payload.size() - m_offset) + QNetworkReply::bytesAvailable();
        }

      protected:
        qint64 readData(char* data, const qint64 maximumSize) override
        {
            if (m_offset >= m_payload.size())
            {
                return -1;
            }
            const auto count = std::min(maximumSize, m_payload.size() - m_offset);
            std::memcpy(data, m_payload.constData() + m_offset, static_cast<size_t>(count));
            m_offset += count;
            return count;
        }

      private:
        QByteArray m_payload;
        qint64 m_offset = 0;
    };

    class TranslatingNetworkAccessManager final : public QNetworkAccessManager
    {
      public:
        QVector<QByteArray> requestBodies;
        QVector<QByteArray> apiKeys;
        QVector<QString> sourceLanguages;
        QVector<QString> targetLanguages;

      protected:
        QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request,
                                     QIODevice* outgoingData) override
        {
            REQUIRE(operation == PostOperation);
            REQUIRE(outgoingData != nullptr);
            requestBodies.push_back(outgoingData->readAll());
            apiKeys.push_back(request.rawHeader(QByteArrayLiteral("X-goog-api-key")));

            const auto document = QJsonDocument::fromJson(requestBodies.back());
            REQUIRE(document.isArray());
            const auto root = document.array();
            REQUIRE(root.size() == 2);
            REQUIRE(root[0].isArray());
            const auto payload = root[0].toArray();
            REQUIRE(payload.size() == 3);
            REQUIRE(payload[0].isArray());
            sourceLanguages.push_back(payload[1].toString());
            targetLanguages.push_back(payload[2].toString());

            QJsonArray translations;
            for (qsizetype index = 0; index < payload[0].toArray().size(); ++index)
            {
                translations.push_back(
                    QStringLiteral("<pre>translated-%1 &amp;</pre>").arg(m_translationIndex++));
            }
            return new ScriptedReply(
                request, QJsonDocument{QJsonArray{translations}}.toJson(QJsonDocument::Compact),
                this);
        }

      private:
        qsizetype m_translationIndex = 0;
    };
} // namespace

TEST_CASE("Google translation backend preserves batching and provider wire format",
          "[translation][google]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    TranslatingNetworkAccessManager network;
    GoogleTranslationBackend backend{network};
    backend.setApiKeyOverride(QStringLiteral(" test-key "));

    const auto result = QCoro::waitFor(backend.translate({
        .sourceLanguage = QStringLiteral("zh-Hant"),
        .targetLanguage = QStringLiteral("en"),
        .texts = {QString(790, QLatin1Char('a')), QString(790, QLatin1Char('b'))},
        .fetchPolicy = ExternalFetchPolicy::AllowExternalFetch,
    }));

    REQUIRE(std::holds_alternative<BackendTranslation>(result));
    const auto& translated = std::get<BackendTranslation>(result);
    CHECK(translated.texts ==
          QVector<QString>{QStringLiteral("translated-0 &"), QStringLiteral("translated-1 &")});
    CHECK(translated.backendRevision == QStringLiteral("google-translate-html-v1"));
    REQUIRE(network.requestBodies.size() == 2);
    CHECK(network.apiKeys ==
          QVector<QByteArray>{QByteArrayLiteral("test-key"), QByteArrayLiteral("test-key")});
    CHECK(network.sourceLanguages ==
          QVector<QString>{QStringLiteral("zh-TW"), QStringLiteral("zh-TW")});
    CHECK(network.targetLanguages == QVector<QString>{QStringLiteral("en"), QStringLiteral("en")});
}

TEST_CASE("Google translation backend refuses implicit external fetches",
          "[translation][google][policy]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    TranslatingNetworkAccessManager network;
    GoogleTranslationBackend backend{network};

    const auto result = QCoro::waitFor(backend.translate({
        .sourceLanguage = QStringLiteral("fr"),
        .targetLanguage = QStringLiteral("en"),
        .texts = {QStringLiteral("Bonjour")},
        .fetchPolicy = ExternalFetchPolicy::InstalledAndCachedOnly,
    }));

    REQUIRE(std::holds_alternative<TranslationUnavailable>(result));
    CHECK(std::get<TranslationUnavailable>(result).reason ==
          TranslationUnavailable::Reason::ExternalFetchNotAllowed);
    CHECK(network.requestBodies.empty());
}
