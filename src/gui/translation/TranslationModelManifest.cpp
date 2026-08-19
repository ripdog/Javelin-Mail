#include "gui/translation/TranslationModelManifest.h"

#include <KLocalizedString>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <memory>
#include <utility>

namespace javelin::gui::translation
{
    namespace
    {
        constexpr auto manifestResource = ":/javelin/translation/manifest-v1.json";
        constexpr auto expectedEngineVersion = "v0.6.0";
        constexpr auto expectedEngineCommit = "4732dc947bc952abb019aabfe5582006d4fc3337";

        [[nodiscard]] bool validHash(const QString& value)
        {
            static const QRegularExpression pattern{QStringLiteral("^[0-9a-f]{64}$")};
            return pattern.match(value).hasMatch();
        }

        [[nodiscard]] TranslationError manifestError(QString message)
        {
            return {
                .code = TranslationErrorCode::ManifestInvalid,
                .message = std::move(message),
            };
        }

        [[nodiscard]] TranslationError invalidField(const char* name)
        {
            return manifestError(i18n("Translation model manifest field '%1' is invalid.",
                                      QString::fromLatin1(name)));
        }

        [[nodiscard]] QString requiredString(const QJsonObject& object, const char* name,
                                             TranslationError& error)
        {
            const auto value = object.value(QLatin1StringView{name});
            if (!value.isString() || value.toString().isEmpty())
            {
                error = invalidField(name);
                return {};
            }
            return value.toString();
        }

        [[nodiscard]] qint64 requiredSize(const QJsonObject& object, const char* name,
                                          TranslationError& error)
        {
            const auto value = object.value(QLatin1StringView{name});
            if (!value.isDouble())
            {
                error = invalidField(name);
                return 0;
            }
            const auto size = value.toInteger();
            if (size <= 0)
                error = invalidField(name);
            return size;
        }

        [[nodiscard]] bool validFileSet(const QVector<TranslationModelFile>& files)
        {
            QSet<QString> types;
            QSet<QString> names;
            for (const auto& file : files)
            {
                if (types.contains(file.type) || names.contains(file.installedName))
                {
                    return false;
                }
                types.insert(file.type);
                names.insert(file.installedName);
            }
            return types.contains(QStringLiteral("model")) &&
                   types.contains(QStringLiteral("lex")) &&
                   (types.contains(QStringLiteral("vocab")) ||
                    (types.contains(QStringLiteral("srcvocab")) &&
                     types.contains(QStringLiteral("trgvocab"))));
        }
    } // namespace

    QString TranslationModelDirection::id() const
    {
        return QStringLiteral("%1-%2").arg(source, target);
    }

    bool TranslationModelRoute::supported() const
    {
        return status != Status::Unsupported;
    }

    bool TranslationModelRoute::isIdentity() const
    {
        return status == Status::Identity;
    }

    std::unique_ptr<TranslationModelManifest>
    TranslationModelManifest::fromJson(QByteArray json, TranslationError& error)
    {
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(json, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        {
            error = manifestError(i18n("The translation model manifest is not valid JSON."));
            return nullptr;
        }
        const auto root = document.object();
        if (root.value(QStringLiteral("schemaVersion")).toInt() != 1)
        {
            error = manifestError(i18n("The translation model manifest schema is unsupported."));
            return nullptr;
        }

        const auto engine = root.value(QStringLiteral("engine")).toObject();
        if (engine.value(QStringLiteral("name")).toString() != QStringLiteral("bergamot") ||
            engine.value(QStringLiteral("version")).toString() !=
                QString::fromLatin1(expectedEngineVersion) ||
            engine.value(QStringLiteral("sourceCommit")).toString() !=
                QString::fromLatin1(expectedEngineCommit))
        {
            error = manifestError(
                i18n("The translation model manifest does not match the bundled engine."));
            return nullptr;
        }

        auto manifest = std::make_unique<TranslationModelManifest>();
        manifest->m_revision = requiredString(root, "manifestRevision", error);
        if (manifest->m_revision.isEmpty())
        {
            return nullptr;
        }

        const auto directionsValue = root.value(QStringLiteral("directions"));
        if (!directionsValue.isArray())
        {
            error = manifestError(i18n("The translation model manifest has no directions."));
            return nullptr;
        }

        QSet<QString> directionIds;
        for (const auto& value : directionsValue.toArray())
        {
            if (!value.isObject())
            {
                error = manifestError(i18n("A translation model direction is invalid."));
                return nullptr;
            }
            const auto object = value.toObject();
            TranslationModelDirection direction{
                .source = canonicalLanguageTag(requiredString(object, "source", error)),
                .target = canonicalLanguageTag(requiredString(object, "target", error)),
                .mozillaSource = requiredString(object, "mozillaSource", error),
                .mozillaTarget = requiredString(object, "mozillaTarget", error),
                .modelVersion = requiredString(object, "modelVersion", error),
                .architecture = requiredString(object, "architecture", error),
                .files = {},
                .licenseFiles = {},
            };
            if (direction.source.isEmpty() || direction.target.isEmpty() ||
                !error.message.isEmpty())
            {
                return nullptr;
            }
            if (direction.source == direction.target || directionIds.contains(direction.id()))
            {
                error = manifestError(
                    i18n("The translation model manifest has duplicate or identity directions."));
                return nullptr;
            }
            directionIds.insert(direction.id());

            const auto filesValue = object.value(QStringLiteral("files"));
            if (!filesValue.isArray())
            {
                error = manifestError(i18n("A translation model direction has no files."));
                return nullptr;
            }
            for (const auto& fileValue : filesValue.toArray())
            {
                const auto fileObject = fileValue.toObject();
                TranslationModelFile file{
                    .type = requiredString(fileObject, "type", error),
                    .url = requiredString(fileObject, "url", error),
                    .compression = requiredString(fileObject, "compression", error),
                    .compressedSize = requiredSize(fileObject, "compressedSize", error),
                    .compressedSha256 =
                        requiredString(fileObject, "compressedSha256", error).toLower(),
                    .decompressedSize = requiredSize(fileObject, "decompressedSize", error),
                    .decompressedSha256 =
                        requiredString(fileObject, "decompressedSha256", error).toLower(),
                    .installedName = requiredString(fileObject, "installedName", error),
                };
                const QUrl url{file.url};
                if (!error.message.isEmpty() || file.compression != QStringLiteral("zstd") ||
                    !url.isValid() || url.scheme() != QStringLiteral("https") ||
                    url.host() != QStringLiteral("firefox-settings-attachments.cdn.mozilla.net") ||
                    !validHash(file.compressedSha256) || !validHash(file.decompressedSha256) ||
                    file.installedName.contains(QLatin1Char('/')) ||
                    file.installedName.contains(QStringLiteral("..")))
                {
                    error = manifestError(i18n("A translation model file is invalid."));
                    return nullptr;
                }
                direction.files.push_back(std::move(file));
            }
            if (!validFileSet(direction.files))
            {
                error = manifestError(i18n("A translation model direction is incomplete."));
                return nullptr;
            }

            const auto licenses = object.value(QStringLiteral("licenseFiles"));
            if (!licenses.isArray())
            {
                error =
                    manifestError(i18n("A translation model direction has no license metadata."));
                return nullptr;
            }
            for (const auto& license : licenses.toArray())
            {
                const auto name = license.toString();
                if (name.isEmpty() || name.contains(QLatin1Char('/')) ||
                    name.contains(QStringLiteral("..")))
                {
                    error = manifestError(i18n("A translation model license entry is invalid."));
                    return nullptr;
                }
                direction.licenseFiles.push_back(name);
            }
            manifest->m_directions.push_back(std::move(direction));
        }
        if (manifest->m_directions.empty())
        {
            error = manifestError(i18n("The translation model manifest is empty."));
            return nullptr;
        }
        std::ranges::sort(manifest->m_directions, {}, &TranslationModelDirection::id);
        return manifest;
    }

    std::unique_ptr<TranslationModelManifest>
    TranslationModelManifest::fromResource(TranslationError& error)
    {
        QFile file{QString::fromLatin1(manifestResource)};
        if (!file.open(QIODevice::ReadOnly))
        {
            error = manifestError(i18n("The bundled translation model manifest is missing."));
            return nullptr;
        }
        return fromJson(file.readAll(), error);
    }

    QString TranslationModelManifest::revision() const
    {
        return m_revision;
    }

    const QVector<TranslationModelDirection>& TranslationModelManifest::directions() const
    {
        return m_directions;
    }

    const TranslationModelDirection*
    TranslationModelManifest::direction(const QStringView source, const QStringView target) const
    {
        const auto canonicalSource = canonicalLanguageTag(source.toString());
        const auto canonicalTarget = canonicalLanguageTag(target.toString());
        const auto found = std::ranges::find_if(
            m_directions, [&](const auto& value)
            { return value.source == canonicalSource && value.target == canonicalTarget; });
        return found == m_directions.end() ? nullptr : &*found;
    }

    TranslationModelRoute TranslationModelManifest::route(const QStringView source,
                                                          const QStringView target) const
    {
        const auto canonicalSource = canonicalLanguageTag(source.toString());
        const auto canonicalTarget = canonicalLanguageTag(target.toString());
        if (canonicalSource.isEmpty() || canonicalTarget.isEmpty())
        {
            return {};
        }
        if (canonicalSource == canonicalTarget)
        {
            return {.status = TranslationModelRoute::Status::Identity, .legs = {}};
        }
        if (const auto* direct = direction(canonicalSource, canonicalTarget))
        {
            return {.status = TranslationModelRoute::Status::Available, .legs = {direct}};
        }
        if (canonicalSource != QStringLiteral("en") && canonicalTarget != QStringLiteral("en"))
        {
            const auto* first = direction(canonicalSource, QStringLiteral("en"));
            const auto* second = direction(QStringLiteral("en"), canonicalTarget);
            if (first != nullptr && second != nullptr)
            {
                return {.status = TranslationModelRoute::Status::Available,
                        .legs = {first, second}};
            }
        }
        return {};
    }

} // namespace javelin::gui::translation
