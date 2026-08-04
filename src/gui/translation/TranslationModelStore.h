#pragma once

#include "gui/translation/TranslationModelManifest.h"

#include <QCoroTask>

#include <QObject>
#include <QString>
#include <QVector>

#include <optional>
#include <variant>

class QNetworkAccessManager;

namespace javelin::gui::translation
{
    struct InstalledTranslationModel
    {
        const TranslationModelDirection* direction = nullptr;
        QString directory;
        qint64 diskSize = 0;
    };

    struct InstalledTranslationRoute
    {
        QVector<InstalledTranslationModel> legs;
    };

    using ModelInstallResult =
        std::variant<InstalledTranslationRoute, TranslationUnavailable, TranslationError>;

    class TranslationModelStore final : public QObject
    {
        Q_OBJECT

      public:
        TranslationModelStore(const TranslationModelManifest& manifest,
                              QNetworkAccessManager& networkAccessManager, QString rootPath = {},
                              QObject* parent = nullptr);

        [[nodiscard]] QString rootPath() const;
        [[nodiscard]] QString directionPath(const TranslationModelDirection& direction) const;
        [[nodiscard]] bool isInstalled(const TranslationModelDirection& direction,
                                       TranslationError* error = nullptr) const;
        [[nodiscard]] QVector<InstalledTranslationModel> installedModels() const;
        [[nodiscard]] QCoro::Task<ModelInstallResult>
        ensureInstalled(TranslationModelRoute route, ExternalFetchPolicy fetchPolicy);
        [[nodiscard]] std::optional<TranslationError>
        remove(const QVector<const TranslationModelDirection*>& directions);
        void cleanupStaleInstallations();

      Q_SIGNALS:
        void downloadProgress(QString direction, qint64 received, qint64 total);
        void installedModelsChanged();

      private:
        [[nodiscard]] QCoro::Task<std::optional<TranslationError>>
        install(TranslationModelDirection direction);
        [[nodiscard]] QCoro::Task<std::optional<TranslationError>>
        downloadAndDecompress(TranslationModelDirection direction, TranslationModelFile file,
                              QString temporaryDirectory);
        [[nodiscard]] bool validateInstalledMetadata(const TranslationModelDirection& direction,
                                                     const QString& directory,
                                                     TranslationError* error) const;
        [[nodiscard]] static QString hashFile(const QString& path, qint64 maximumSize,
                                              qint64* observedSize, TranslationError* error);
        [[nodiscard]] static std::optional<TranslationError>
        decompressZstd(const QString& compressedPath, const QString& outputPath, qint64 maximumSize,
                       qint64 expectedSize, QStringView expectedHash);
        [[nodiscard]] static std::optional<TranslationError>
        writeInstalledMetadata(const QString& directory, const TranslationModelManifest& manifest,
                               const TranslationModelDirection& direction);
        [[nodiscard]] static TranslationError modelError(TranslationErrorCode code,
                                                         QString message);

        const TranslationModelManifest& m_manifest;
        QNetworkAccessManager& m_networkAccessManager;
        QString m_rootPath;
    };
} // namespace javelin::gui::translation
