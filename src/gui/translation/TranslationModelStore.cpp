#include "gui/translation/TranslationModelStore.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif
#include <QCoroNetworkReply>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>

#include <zstd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <utility>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace javelin::gui::translation
{
    namespace
    {
        constexpr int downloadTimeoutMs = 120000;
        constexpr qint64 streamChunkSize = 256 * 1024;

        [[nodiscard]] QString defaultModelRoot()
        {
            return QDir{QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)}
                .filePath(QStringLiteral("translations/models/v1"));
        }

        [[nodiscard]] bool syncFile(QFile& file)
        {
            if (!file.flush())
            {
                return false;
            }
#ifdef Q_OS_UNIX
            return file.handle() < 0 || ::fsync(file.handle()) == 0;
#else
            return true;
#endif
        }

        [[nodiscard]] qint64 directorySize(const QString& path)
        {
            qint64 size = 0;
            QDir directory{path};
            const auto entries = directory.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
            for (const auto& entry : entries)
            {
                size += entry.size();
            }
            return size;
        }
    } // namespace

    TranslationModelStore::TranslationModelStore(const TranslationModelManifest& manifest,
                                                 QNetworkAccessManager& networkAccessManager,
                                                 QString rootPath, QObject* parent)
        : QObject(parent), m_manifest(manifest), m_networkAccessManager(networkAccessManager),
          m_rootPath(rootPath.isEmpty() ? defaultModelRoot() : std::move(rootPath))
    {
        cleanupStaleInstallations();
    }

    QString TranslationModelStore::rootPath() const
    {
        return m_rootPath;
    }

    QString TranslationModelStore::directionPath(const TranslationModelDirection& direction) const
    {
        return QDir{m_rootPath}.filePath(
            QStringLiteral("%1/%2-%3")
                .arg(direction.id(), direction.modelVersion, direction.architecture));
    }

    bool TranslationModelStore::isInstalled(const TranslationModelDirection& direction,
                                            TranslationError* error) const
    {
        return validateInstalledMetadata(direction, directionPath(direction), error);
    }

    QVector<InstalledTranslationModel> TranslationModelStore::installedModels() const
    {
        QVector<InstalledTranslationModel> result;
        for (const auto& direction : m_manifest.directions())
        {
            TranslationError error;
            const auto directory = directionPath(direction);
            if (validateInstalledMetadata(direction, directory, &error))
            {
                result.push_back({
                    .direction = &direction,
                    .directory = directory,
                    .diskSize = directorySize(directory),
                });
            }
        }
        return result;
    }

    QCoro::Task<ModelInstallResult>
    TranslationModelStore::ensureInstalled(TranslationModelRoute route,
                                           const ExternalFetchPolicy fetchPolicy)
    {
        if (route.legs.empty())
        {
            co_return TranslationUnavailable{
                .reason = TranslationUnavailable::Reason::UnsupportedLanguageRoute,
            };
        }

        InstalledTranslationRoute installed;
        installed.legs.reserve(route.legs.size());
        for (const auto* direction : route.legs)
        {
            TranslationError validationError;
            if (!isInstalled(*direction, &validationError))
            {
                if (fetchPolicy != ExternalFetchPolicy::AllowExternalFetch)
                {
                    co_return TranslationUnavailable{
                        .reason = TranslationUnavailable::Reason::RequiredModelNotInstalled,
                    };
                }
                if (const auto installError = co_await install(*direction))
                {
                    co_return *installError;
                }
                if (!isInstalled(*direction, &validationError))
                {
                    co_return validationError;
                }
            }
            const auto directory = directionPath(*direction);
            installed.legs.push_back({
                .direction = direction,
                .directory = directory,
                .diskSize = directorySize(directory),
            });
        }
        co_return installed;
    }

    std::optional<TranslationError>
    TranslationModelStore::remove(const QVector<const TranslationModelDirection*>& directions)
    {
        const QDir root{QFileInfo{m_rootPath}.canonicalFilePath()};
        if (!root.exists())
        {
            return std::nullopt;
        }
        for (const auto* direction : directions)
        {
            if (direction == nullptr)
            {
                continue;
            }
            const QFileInfo info{directionPath(*direction)};
            if (!info.exists())
            {
                continue;
            }
            const auto canonical = info.canonicalFilePath();
            if (canonical.isEmpty() ||
                !canonical.startsWith(root.canonicalPath() + QLatin1Char('/')))
            {
                return modelError(
                    TranslationErrorCode::ModelVerificationFailed,
                    QStringLiteral("Refusing to remove a model outside the model store."));
            }
            if (!validateInstalledMetadata(*direction, canonical, nullptr))
            {
                return modelError(
                    TranslationErrorCode::ModelVerificationFailed,
                    QStringLiteral("Refusing to remove an unrecognised model directory."));
            }
            QDir directory{canonical};
            if (!directory.removeRecursively())
            {
                return modelError(
                    TranslationErrorCode::ModelVerificationFailed,
                    QStringLiteral("Could not remove the installed translation model."));
            }
        }
        Q_EMIT installedModelsChanged();
        return std::nullopt;
    }

    void TranslationModelStore::cleanupStaleInstallations()
    {
        QDir root{m_rootPath};
        if (!root.exists())
        {
            return;
        }
        const auto entries =
            root.entryInfoList({QStringLiteral(".install-*"), QStringLiteral(".previous-*")},
                               QDir::Dirs | QDir::NoDotAndDotDot);
        for (const auto& entry : entries)
        {
            QDir{entry.absoluteFilePath()}.removeRecursively();
        }
    }

    QCoro::Task<std::optional<TranslationError>>
    TranslationModelStore::install(TranslationModelDirection direction)
    {
        if (!QDir{}.mkpath(m_rootPath))
        {
            co_return modelError(
                TranslationErrorCode::ModelDownloadFailed,
                QStringLiteral("Could not create the translation model directory."));
        }
        const auto temporaryDirectory = QDir{m_rootPath}.filePath(
            QStringLiteral(".install-%1-%2")
                .arg(direction.id(), QUuid::createUuid().toString(QUuid::WithoutBraces)));
        if (!QDir{}.mkpath(temporaryDirectory))
        {
            co_return modelError(TranslationErrorCode::ModelDownloadFailed,
                                 QStringLiteral("Could not create a temporary model directory."));
        }
        auto cleanup =
            qScopeGuard([&temporaryDirectory]() { QDir{temporaryDirectory}.removeRecursively(); });

        for (const auto& file : direction.files)
        {
            if (const auto error =
                    co_await downloadAndDecompress(direction, file, temporaryDirectory))
            {
                co_return error;
            }
        }
        if (const auto error = writeInstalledMetadata(temporaryDirectory, m_manifest, direction))
        {
            co_return error;
        }

        const auto finalDirectory = directionPath(direction);
        const auto parentDirectory = QFileInfo{finalDirectory}.absolutePath();
        if (!QDir{}.mkpath(parentDirectory))
        {
            co_return modelError(TranslationErrorCode::ModelVerificationFailed,
                                 QStringLiteral("Could not create the model destination."));
        }
        const auto previousDirectory = QDir{m_rootPath}.filePath(
            QStringLiteral(".previous-%1-%2")
                .arg(direction.id(), QUuid::createUuid().toString(QUuid::WithoutBraces)));
        const bool hadPrevious = QFileInfo::exists(finalDirectory);
        if (hadPrevious && !QDir{}.rename(finalDirectory, previousDirectory))
        {
            co_return modelError(
                TranslationErrorCode::ModelVerificationFailed,
                QStringLiteral("Could not replace the installed translation model."));
        }
        if (!QDir{}.rename(temporaryDirectory, finalDirectory))
        {
            if (hadPrevious)
            {
                QDir{}.rename(previousDirectory, finalDirectory);
            }
            co_return modelError(
                TranslationErrorCode::ModelVerificationFailed,
                QStringLiteral("Could not activate the downloaded translation model."));
        }
        cleanup.dismiss();
        if (hadPrevious)
        {
            QDir{previousDirectory}.removeRecursively();
        }
        Q_EMIT installedModelsChanged();
        co_return std::nullopt;
    }

    QCoro::Task<std::optional<TranslationError>> TranslationModelStore::downloadAndDecompress(
        TranslationModelDirection direction, TranslationModelFile file, QString temporaryDirectory)
    {
        const auto compressedPath =
            QDir{temporaryDirectory}.filePath(file.installedName + QStringLiteral(".zst.part"));
        QFile compressed{compressedPath};
        if (!compressed.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            co_return modelError(TranslationErrorCode::ModelDownloadFailed,
                                 QStringLiteral("Could not create a model download file."));
        }

        QNetworkRequest request{QUrl{file.url}};
        request.setTransferTimeout(downloadTimeoutMs);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        auto* reply = m_networkAccessManager.get(request);
        const auto deleteReply = qScopeGuard([reply]() { reply->deleteLater(); });
        qint64 received = 0;
        bool writeFailed = false;
        const auto drain = [&]
        {
            const auto data = reply->readAll();
            if (data.isEmpty())
            {
                return;
            }
            received += data.size();
            if (received > file.compressedSize || compressed.write(data) != data.size())
            {
                writeFailed = true;
                reply->abort();
                return;
            }
            Q_EMIT downloadProgress(direction.id(), received, file.compressedSize);
        };
        const auto readyConnection = connect(reply, &QNetworkReply::readyRead, this, drain);
        co_await qCoro(reply).waitForFinished();
        disconnect(readyConnection);
        drain();
        if (reply->error() != QNetworkReply::NoError || writeFailed)
        {
            compressed.close();
            co_return modelError(
                TranslationErrorCode::ModelDownloadFailed,
                writeFailed ? QStringLiteral("The model download exceeded its declared size.")
                            : reply->errorString());
        }
        if (received != file.compressedSize || !syncFile(compressed))
        {
            compressed.close();
            co_return modelError(TranslationErrorCode::ModelVerificationFailed,
                                 QStringLiteral("The downloaded model has an unexpected size."));
        }
        compressed.close();

        qint64 compressedSize = 0;
        TranslationError hashError;
        const auto compressedHash =
            hashFile(compressedPath, file.compressedSize, &compressedSize, &hashError);
        if (!hashError.message.isEmpty() || compressedSize != file.compressedSize ||
            compressedHash != file.compressedSha256)
        {
            co_return modelError(TranslationErrorCode::ModelVerificationFailed,
                                 QStringLiteral("The downloaded model failed verification."));
        }

        const auto outputPath = QDir{temporaryDirectory}.filePath(file.installedName);
        if (const auto error = decompressZstd(compressedPath, outputPath, file.decompressedSize,
                                              file.decompressedSize, file.decompressedSha256))
        {
            co_return error;
        }
        QFile::remove(compressedPath);
        co_return std::nullopt;
    }

    bool
    TranslationModelStore::validateInstalledMetadata(const TranslationModelDirection& direction,
                                                     const QString& directory,
                                                     TranslationError* error) const
    {
        QFile metadata{QDir{directory}.filePath(QStringLiteral("installed.json"))};
        if (!metadata.open(QIODevice::ReadOnly))
        {
            if (error != nullptr)
            {
                *error =
                    modelError(TranslationErrorCode::ModelVerificationFailed,
                               QStringLiteral("The required translation model is not installed."));
            }
            return false;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(metadata.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        {
            if (error != nullptr)
            {
                *error = modelError(TranslationErrorCode::ModelVerificationFailed,
                                    QStringLiteral("The installed translation model is corrupt."));
            }
            return false;
        }
        const auto root = document.object();
        if (root.value(QStringLiteral("manifestRevision")).toString() != m_manifest.revision() ||
            root.value(QStringLiteral("source")).toString() != direction.source ||
            root.value(QStringLiteral("target")).toString() != direction.target ||
            root.value(QStringLiteral("modelVersion")).toString() != direction.modelVersion ||
            root.value(QStringLiteral("architecture")).toString() != direction.architecture)
        {
            if (error != nullptr)
            {
                *error = modelError(TranslationErrorCode::ModelVerificationFailed,
                                    QStringLiteral("The installed translation model is obsolete."));
            }
            return false;
        }

        for (const auto& file : direction.files)
        {
            qint64 observedSize = 0;
            TranslationError hashError;
            const auto hash = hashFile(QDir{directory}.filePath(file.installedName),
                                       file.decompressedSize, &observedSize, &hashError);
            if (!hashError.message.isEmpty() || observedSize != file.decompressedSize ||
                hash != file.decompressedSha256)
            {
                if (error != nullptr)
                {
                    *error =
                        modelError(TranslationErrorCode::ModelVerificationFailed,
                                   QStringLiteral("The installed translation model is corrupt."));
                }
                return false;
            }
        }
        return true;
    }

    QString TranslationModelStore::hashFile(const QString& path, const qint64 maximumSize,
                                            qint64* observedSize, TranslationError* error)
    {
        QFile file{path};
        if (!file.open(QIODevice::ReadOnly))
        {
            if (error != nullptr)
            {
                *error = modelError(TranslationErrorCode::ModelVerificationFailed,
                                    QStringLiteral("Could not read an installed model file."));
            }
            return {};
        }
        QCryptographicHash hash{QCryptographicHash::Sha256};
        qint64 total = 0;
        while (!file.atEnd())
        {
            const auto data = file.read(streamChunkSize);
            if (data.isNull())
            {
                if (error != nullptr)
                {
                    *error = modelError(TranslationErrorCode::ModelVerificationFailed,
                                        QStringLiteral("Could not read an installed model file."));
                }
                return {};
            }
            total += data.size();
            if (total > maximumSize)
            {
                if (error != nullptr)
                {
                    *error = modelError(TranslationErrorCode::ModelVerificationFailed,
                                        QStringLiteral("An installed model file is too large."));
                }
                return {};
            }
            hash.addData(data);
        }
        if (observedSize != nullptr)
        {
            *observedSize = total;
        }
        return QString::fromLatin1(hash.result().toHex());
    }

    std::optional<TranslationError>
    TranslationModelStore::decompressZstd(const QString& compressedPath, const QString& outputPath,
                                          const qint64 maximumSize, const qint64 expectedSize,
                                          const QStringView expectedHash)
    {
        QFile input{compressedPath};
        QFile output{outputPath + QStringLiteral(".part")};
        if (!input.open(QIODevice::ReadOnly) ||
            !output.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            return modelError(TranslationErrorCode::ModelVerificationFailed,
                              QStringLiteral("Could not open translation model files."));
        }

        auto* stream = ZSTD_createDStream();
        if (stream == nullptr)
        {
            return modelError(TranslationErrorCode::ModelVerificationFailed,
                              QStringLiteral("Could not initialize model decompression."));
        }
        const auto freeStream = qScopeGuard([stream]() { ZSTD_freeDStream(stream); });
        auto result = ZSTD_initDStream(stream);
        if (ZSTD_isError(result) != 0U)
        {
            return modelError(TranslationErrorCode::ModelVerificationFailed,
                              QStringLiteral("Could not initialize model decompression."));
        }

        QByteArray inputBuffer;
        inputBuffer.resize(static_cast<qsizetype>(ZSTD_DStreamInSize()));
        QByteArray outputBuffer;
        outputBuffer.resize(static_cast<qsizetype>(ZSTD_DStreamOutSize()));
        QCryptographicHash hash{QCryptographicHash::Sha256};
        qint64 written = 0;
        size_t remaining = 1;
        while (!input.atEnd())
        {
            const auto count = input.read(inputBuffer.data(), inputBuffer.size());
            if (count <= 0)
            {
                return modelError(
                    TranslationErrorCode::ModelVerificationFailed,
                    QStringLiteral("Could not read the compressed translation model."));
            }
            ZSTD_inBuffer zstdInput{inputBuffer.constData(), static_cast<size_t>(count), 0};
            while (zstdInput.pos < zstdInput.size)
            {
                ZSTD_outBuffer zstdOutput{outputBuffer.data(),
                                          static_cast<size_t>(outputBuffer.size()), 0};
                remaining = ZSTD_decompressStream(stream, &zstdOutput, &zstdInput);
                if (ZSTD_isError(remaining) != 0U)
                {
                    return modelError(
                        TranslationErrorCode::ModelVerificationFailed,
                        QStringLiteral("The translation model could not be decompressed."));
                }
                const auto produced = static_cast<qint64>(zstdOutput.pos);
                written += produced;
                if (written > maximumSize ||
                    output.write(outputBuffer.constData(), produced) != produced)
                {
                    return modelError(
                        TranslationErrorCode::ModelVerificationFailed,
                        QStringLiteral("The decompressed translation model is invalid."));
                }
                hash.addData(QByteArrayView{outputBuffer.constData(), produced});
            }
        }
        if (remaining != 0 || written != expectedSize ||
            QString::fromLatin1(hash.result().toHex()) != expectedHash || !syncFile(output))
        {
            return modelError(
                TranslationErrorCode::ModelVerificationFailed,
                QStringLiteral("The decompressed translation model failed verification."));
        }
        output.close();
        QFile::remove(outputPath);
        if (!QFile::rename(output.fileName(), outputPath))
        {
            return modelError(
                TranslationErrorCode::ModelVerificationFailed,
                QStringLiteral("Could not finish installing a translation model file."));
        }
        return std::nullopt;
    }

    std::optional<TranslationError>
    TranslationModelStore::writeInstalledMetadata(const QString& directory,
                                                  const TranslationModelManifest& manifest,
                                                  const TranslationModelDirection& direction)
    {
        for (const auto& licenseName : direction.licenseFiles)
        {
            QFile source{QStringLiteral(":/javelin/translation/licenses/%1").arg(licenseName)};
            QSaveFile destination{QDir{directory}.filePath(licenseName)};
            if (!source.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly))
            {
                return modelError(
                    TranslationErrorCode::ModelVerificationFailed,
                    QStringLiteral("Could not install translation model licence metadata."));
            }
            const auto contents = source.readAll();
            if (contents.isEmpty() || destination.write(contents) != contents.size() ||
                !destination.commit())
            {
                return modelError(
                    TranslationErrorCode::ModelVerificationFailed,
                    QStringLiteral("Could not install translation model licence metadata."));
            }
        }

        QJsonArray files;
        for (const auto& file : direction.files)
        {
            files.push_back(QJsonObject{
                {QStringLiteral("name"), file.installedName},
                {QStringLiteral("size"), file.decompressedSize},
                {QStringLiteral("sha256"), file.decompressedSha256},
            });
        }
        const QJsonObject root{
            {QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("manifestRevision"), manifest.revision()},
            {QStringLiteral("source"), direction.source},
            {QStringLiteral("target"), direction.target},
            {QStringLiteral("modelVersion"), direction.modelVersion},
            {QStringLiteral("architecture"), direction.architecture},
            {QStringLiteral("installedAt"),
             QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {QStringLiteral("files"), files},
        };
        QSaveFile file{QDir{directory}.filePath(QStringLiteral("installed.json"))};
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(QJsonDocument{root}.toJson(QJsonDocument::Indented)) < 0 || !file.commit())
        {
            return modelError(TranslationErrorCode::ModelVerificationFailed,
                              QStringLiteral("Could not record the installed translation model."));
        }
        return std::nullopt;
    }

    TranslationError TranslationModelStore::modelError(const TranslationErrorCode code,
                                                       QString message)
    {
        return {
            .code = code,
            .message = std::move(message),
        };
    }
} // namespace javelin::gui::translation
