#pragma once

#include "gui/translation/TranslationTypes.h"

#include <QString>
#include <QVector>

#include <memory>

namespace javelin::gui::translation
{
    struct TranslationModelFile
    {
        QString type;
        QString url;
        QString compression;
        qint64 compressedSize = 0;
        QString compressedSha256;
        qint64 decompressedSize = 0;
        QString decompressedSha256;
        QString installedName;
    };

    struct TranslationModelDirection
    {
        QString source;
        QString target;
        QString mozillaSource;
        QString mozillaTarget;
        QString modelVersion;
        QString architecture;
        QVector<TranslationModelFile> files;
        QStringList licenseFiles;

        [[nodiscard]] QString id() const;
    };

    struct TranslationModelRoute
    {
        enum class Status
        {
            Unsupported,
            Identity,
            Available,
        };

        Status status = Status::Unsupported;
        QVector<const TranslationModelDirection*> legs;

        [[nodiscard]] bool supported() const;
        [[nodiscard]] bool isIdentity() const;
    };

    class TranslationModelManifest
    {
      public:
        [[nodiscard]] static std::unique_ptr<TranslationModelManifest>
        fromJson(QByteArray json, TranslationError& error);
        [[nodiscard]] static std::unique_ptr<TranslationModelManifest>
        fromResource(TranslationError& error);

        [[nodiscard]] QString revision() const;
        [[nodiscard]] const QVector<TranslationModelDirection>& directions() const;
        [[nodiscard]] const TranslationModelDirection* direction(QStringView source,
                                                                 QStringView target) const;
        [[nodiscard]] TranslationModelRoute route(QStringView source, QStringView target) const;

      private:
        QString m_revision;
        QVector<TranslationModelDirection> m_directions;
    };
} // namespace javelin::gui::translation
