#include "gui/translation/TranslationModelStore.h"
#include "gui/translation/TranslationModelManifest.h"

#include <QCoroTask>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTemporaryDir>
#include <QTimer>

#include <zstd.h>

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
            static char appName[] = "javelin-model-store-tests";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    [[nodiscard]] QString sha256(const QByteArrayView data)
    {
        return QString::fromLatin1(
            QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    }

    [[nodiscard]] QByteArray compressZstd(const QByteArrayView data)
    {
        QByteArray compressed;
        compressed.resize(
            static_cast<qsizetype>(ZSTD_compressBound(static_cast<size_t>(data.size()))));
        const auto size = ZSTD_compress(compressed.data(), static_cast<size_t>(compressed.size()),
                                        data.data(), static_cast<size_t>(data.size()), 3);
        REQUIRE(ZSTD_isError(size) == 0U);
        compressed.resize(static_cast<qsizetype>(size));
        return compressed;
    }

    class ScriptedReply final : public QNetworkReply
    {
      public:
        ScriptedReply(QNetworkRequest request, QByteArray payload, QObject* parent)
            : QNetworkReply(parent), m_payload(std::move(payload))
        {
            setRequest(request);
            setUrl(request.url());
            setHeader(QNetworkRequest::ContentLengthHeader, m_payload.size());
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

    class ScriptedNetworkAccessManager final : public QNetworkAccessManager
    {
      public:
        void add(const QUrl& url, QByteArray payload)
        {
            m_payloads.insert(url, std::move(payload));
        }

      protected:
        QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request,
                                     QIODevice* outgoingData) override
        {
            Q_UNUSED(operation);
            Q_UNUSED(outgoingData);
            const auto found = m_payloads.constFind(request.url());
            if (found == m_payloads.cend())
            {
                return new ScriptedReply(request, {}, this);
            }
            return new ScriptedReply(request, *found, this);
        }

      private:
        QHash<QUrl, QByteArray> m_payloads;
    };

    struct ModelFixture
    {
        QString type;
        QString installedName;
        QUrl url;
        QByteArray uncompressed;
        QByteArray compressed;
    };

    [[nodiscard]] std::unique_ptr<TranslationModelManifest>
    makeManifest(const QVector<ModelFixture>& files)
    {
        QJsonArray fileObjects;
        for (const auto& file : files)
        {
            fileObjects.push_back(QJsonObject{
                {QStringLiteral("type"), file.type},
                {QStringLiteral("url"), file.url.toString()},
                {QStringLiteral("compression"), QStringLiteral("zstd")},
                {QStringLiteral("compressedSize"), file.compressed.size()},
                {QStringLiteral("compressedSha256"), sha256(file.compressed)},
                {QStringLiteral("decompressedSize"), file.uncompressed.size()},
                {QStringLiteral("decompressedSha256"), sha256(file.uncompressed)},
                {QStringLiteral("installedName"), file.installedName},
            });
        }
        const QJsonObject root{
            {QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("manifestRevision"), QStringLiteral("test-manifest-1")},
            {QStringLiteral("engine"),
             QJsonObject{
                 {QStringLiteral("name"), QStringLiteral("bergamot")},
                 {QStringLiteral("version"), QStringLiteral("v0.6.0")},
                 {QStringLiteral("sourceCommit"),
                  QStringLiteral("4732dc947bc952abb019aabfe5582006d4fc3337")},
             }},
            {QStringLiteral("directions"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("source"), QStringLiteral("fr")},
                 {QStringLiteral("target"), QStringLiteral("en")},
                 {QStringLiteral("mozillaSource"), QStringLiteral("fr")},
                 {QStringLiteral("mozillaTarget"), QStringLiteral("en")},
                 {QStringLiteral("modelVersion"), QStringLiteral("3.1")},
                 {QStringLiteral("architecture"), QStringLiteral("base-memory")},
                 {QStringLiteral("files"), fileObjects},
                 {QStringLiteral("licenseFiles"), QJsonArray{QStringLiteral("MPL-2.0.txt")}},
             }}},
        };
        TranslationError error;
        auto manifest = TranslationModelManifest::fromJson(QJsonDocument{root}.toJson(), error);
        INFO(error.message.toStdString());
        REQUIRE(manifest != nullptr);
        return manifest;
    }

    [[nodiscard]] QVector<ModelFixture> modelFixtures()
    {
        QVector<ModelFixture> files{
            {.type = QStringLiteral("model"),
             .installedName = QStringLiteral("model.bin"),
             .url = QUrl{QStringLiteral(
                 "https://firefox-settings-attachments.cdn.mozilla.net/test/model.zst")},
             .uncompressed = QByteArrayLiteral("model-data"),
             .compressed = {}},
            {.type = QStringLiteral("lex"),
             .installedName = QStringLiteral("lex.bin"),
             .url = QUrl{QStringLiteral(
                 "https://firefox-settings-attachments.cdn.mozilla.net/test/lex.zst")},
             .uncompressed = QByteArrayLiteral("lex-data"),
             .compressed = {}},
            {.type = QStringLiteral("vocab"),
             .installedName = QStringLiteral("vocab.spm"),
             .url = QUrl{QStringLiteral(
                 "https://firefox-settings-attachments.cdn.mozilla.net/test/vocab.zst")},
             .uncompressed = QByteArrayLiteral("vocab-data"),
             .compressed = {}},
        };
        for (auto& file : files)
        {
            file.compressed = compressZstd(file.uncompressed);
        }
        return files;
    }
} // namespace

TEST_CASE("translation model store verifies and atomically installs downloaded files",
          "[translation][models]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    const auto files = modelFixtures();
    auto manifest = makeManifest(files);
    ScriptedNetworkAccessManager network;
    for (const auto& file : files)
    {
        network.add(file.url, file.compressed);
    }
    QTemporaryDir modelRoot;
    REQUIRE(modelRoot.isValid());
    TranslationModelStore store{*manifest, network, modelRoot.path()};
    QVector<std::pair<qint64, qint64>> progress;
    QObject::connect(&store, &TranslationModelStore::downloadProgress,
                     [&progress](const QString&, const QString&, const qint64 received,
                                 const qint64 total) { progress.push_back({received, total}); });

    const auto route = manifest->route(QStringLiteral("fr"), QStringLiteral("en"));
    REQUIRE(route.supported());
    const auto installed =
        QCoro::waitFor(store.ensureInstalled(route, ExternalFetchPolicy::AllowExternalFetch));
    REQUIRE(std::holds_alternative<InstalledTranslationRoute>(installed));
    REQUIRE(std::get<InstalledTranslationRoute>(installed).legs.size() == 1);
    REQUIRE_FALSE(progress.empty());
    qint64 expectedDownloadSize = 0;
    for (const auto& file : files)
    {
        expectedDownloadSize += file.compressed.size();
    }
    CHECK(progress.back().first == expectedDownloadSize);
    CHECK(progress.back().second == expectedDownloadSize);
    const auto* direction = manifest->direction(QStringLiteral("fr"), QStringLiteral("en"));
    REQUIRE(direction != nullptr);
    CHECK(store.isInstalled(*direction));
    for (const auto& file : files)
    {
        QFile installedFile{QDir{store.directionPath(*direction)}.filePath(file.installedName)};
        REQUIRE(installedFile.open(QIODevice::ReadOnly));
        CHECK(installedFile.readAll() == file.uncompressed);
    }
    CHECK(QFileInfo::exists(
        QDir{store.directionPath(*direction)}.filePath(QStringLiteral("installed.json"))));
}

TEST_CASE("translation model store rejects corruption and removes only validated installs",
          "[translation][models]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    const auto files = modelFixtures();
    auto manifest = makeManifest(files);
    ScriptedNetworkAccessManager network;
    for (const auto& file : files)
    {
        network.add(file.url, file.compressed);
    }
    QTemporaryDir modelRoot;
    REQUIRE(modelRoot.isValid());
    TranslationModelStore store{*manifest, network, modelRoot.path()};
    const auto route = manifest->route(QStringLiteral("fr"), QStringLiteral("en"));
    const auto installed =
        QCoro::waitFor(store.ensureInstalled(route, ExternalFetchPolicy::AllowExternalFetch));
    REQUIRE(std::holds_alternative<InstalledTranslationRoute>(installed));
    const auto* direction = manifest->direction(QStringLiteral("fr"), QStringLiteral("en"));
    REQUIRE(direction != nullptr);

    QFile corrupt{QDir{store.directionPath(*direction)}.filePath(QStringLiteral("model.bin"))};
    REQUIRE(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
    REQUIRE(corrupt.write("corrupt") == 7);
    corrupt.close();
    TranslationError error;
    CHECK_FALSE(store.isInstalled(*direction, &error));
    CHECK(error.code == TranslationErrorCode::ModelVerificationFailed);
    CHECK(store.remove({direction}).has_value());
    CHECK(QFileInfo::exists(store.directionPath(*direction)));
}

TEST_CASE("translation model store does not fetch models for cache-only requests",
          "[translation][models][policy]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    const auto files = modelFixtures();
    auto manifest = makeManifest(files);
    ScriptedNetworkAccessManager network;
    QTemporaryDir modelRoot;
    REQUIRE(modelRoot.isValid());
    TranslationModelStore store{*manifest, network, modelRoot.path()};

    const auto result = QCoro::waitFor(
        store.ensureInstalled(manifest->route(QStringLiteral("fr"), QStringLiteral("en")),
                              ExternalFetchPolicy::InstalledAndCachedOnly));
    REQUIRE(std::holds_alternative<TranslationUnavailable>(result));
    CHECK(std::get<TranslationUnavailable>(result).reason ==
          TranslationUnavailable::Reason::RequiredModelNotInstalled);
}
