#include "app/MailImportPlanner.h"

#include "app/MailImportSource.h"

#include <KLocalizedString>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUuid>

#include <algorithm>
#include <ranges>
#include <set>
#include <utility>

namespace javelin::app
{
    namespace
    {
        using OperationError = javelin::jmap::OperationError;
        using OperationErrorCode = javelin::jmap::OperationErrorCode;

        [[nodiscard]] OperationError invalidImport(QString message)
        {
            return {.code = OperationErrorCode::InvalidUserInput, .message = std::move(message)};
        }

        [[nodiscard]] QString cleanedRelativePath(QString relative)
        {
            relative = QDir::cleanPath(relative);
            return relative == QStringLiteral(".") ? QString{} : relative;
        }

        [[nodiscard]] QString withoutMailboxExtension(QString relative)
        {
            const QFileInfo info{relative};
            const auto suffix = info.suffix().toCaseFolded();
            if (suffix != QStringLiteral("mbox") && suffix != QStringLiteral("mbx"))
                return relative;
            const auto directory = info.path() == QStringLiteral(".") ? QString{} : info.path();
            const auto base = info.completeBaseName();
            return directory.isEmpty() ? base : QDir{directory}.filePath(base);
        }

        [[nodiscard]] qsizetype pathDepth(const QString& path)
        {
            if (path.isEmpty())
                return 0;
            return path.count(QLatin1Char('/')) + 1;
        }

        [[nodiscard]] std::vector<QString> pathPrefixes(const QString& relativePath)
        {
            std::vector<QString> result;
            const auto parts = relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            QString current;
            for (const auto& part : parts)
            {
                current = current.isEmpty() ? part : QDir{current}.filePath(part);
                result.push_back(current);
            }
            return result;
        }

        enum class JavelinExportFormat
        {
            Eml,
            MboxRd,
        };

        struct JavelinExportMetadata
        {
            JavelinExportFormat format = JavelinExportFormat::Eml;
            QString status;
        };

        [[nodiscard]] std::variant<std::optional<JavelinExportMetadata>, OperationError>
        javelinExportMetadata(const QString& root)
        {
            const auto markerPath =
                QDir{root}.filePath(QStringLiteral(".javelin-mail-export.json"));
            const QFileInfo markerInfo{markerPath};
            if (!markerInfo.exists())
                return std::optional<JavelinExportMetadata>{};
            if (!markerInfo.isFile() || !markerInfo.isReadable() || markerInfo.isSymLink())
                return invalidImport(
                    i18n("Javelin export metadata is not readable: %1", markerPath));
            constexpr qint64 maximumMarkerBytes = 64 * 1024;
            if (markerInfo.size() < 0 || markerInfo.size() > maximumMarkerBytes)
                return invalidImport(i18n("Javelin export metadata is invalid: %1", markerPath));

            QFile marker{markerPath};
            if (!marker.open(QIODevice::ReadOnly))
                return invalidImport(
                    i18n("Could not read Javelin export metadata: %1", markerPath));
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(marker.readAll(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
                return invalidImport(i18n("Javelin export metadata is invalid: %1", markerPath));
            const auto object = document.object();
            if (object.value(QStringLiteral("version")).toInt(-1) != 1)
                return invalidImport(i18n("This Javelin mail export version is not supported."));
            const auto formatName = object.value(QStringLiteral("format")).toString();
            const auto status = object.value(QStringLiteral("status")).toString();
            if (formatName != QStringLiteral("eml") && formatName != QStringLiteral("mboxrd"))
                return invalidImport(i18n("Javelin export metadata has an unknown mail format."));
            if (status != QStringLiteral("complete") && status != QStringLiteral("partial"))
            {
                return invalidImport(
                    i18n("This Javelin mail export did not finish and cannot be imported safely."));
            }
            return std::optional<JavelinExportMetadata>{JavelinExportMetadata{
                .format = formatName == QStringLiteral("mboxrd") ? JavelinExportFormat::MboxRd
                                                                 : JavelinExportFormat::Eml,
                .status = status,
            }};
        }

        struct EnumeratedImportTree
        {
            std::vector<QString> files;
            std::vector<QString> directories;
        };

        [[nodiscard]] std::variant<EnumeratedImportTree, OperationError>
        enumerateImportTree(const QString& root)
        {
            const QFileInfo rootInfo{root};
            if (!rootInfo.exists() || !rootInfo.isDir() || rootInfo.isSymLink())
            {
                return invalidImport(
                    i18n("Import source directory is unavailable or is a symbolic link: %1", root));
            }
            EnumeratedImportTree tree;
            const auto markerPath =
                QFileInfo{QDir{root}.filePath(QStringLiteral(".javelin-mail-export.json"))}
                    .absoluteFilePath();
            QDirIterator iterator{
                root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                QDirIterator::Subdirectories};
            while (iterator.hasNext())
            {
                const auto path = iterator.next();
                const QFileInfo info{path};
                if (info.isSymLink())
                    return invalidImport(i18n("Import source contains a symbolic link: %1", path));
                if (info.isDir())
                {
                    tree.directories.push_back(info.absoluteFilePath());
                    continue;
                }
                if (!info.isFile())
                    continue;
                if (info.absoluteFilePath() == markerPath)
                    continue;
                if (info.fileName().endsWith(QStringLiteral(".javelin-part")))
                {
                    return invalidImport(
                        i18n("Import source contains an unfinished Javelin export file: %1", path));
                }
                tree.files.push_back(info.absoluteFilePath());
            }
            const auto lessByRelativeDepth = [&root](const QString& left, const QString& right)
            {
                const auto leftRelative = QDir{root}.relativeFilePath(left);
                const auto rightRelative = QDir{root}.relativeFilePath(right);
                const auto leftDepth = pathDepth(leftRelative);
                const auto rightDepth = pathDepth(rightRelative);
                return leftDepth == rightDepth ? leftRelative < rightRelative
                                               : leftDepth < rightDepth;
            };
            std::ranges::sort(tree.files, lessByRelativeDepth);
            std::ranges::sort(tree.directories, lessByRelativeDepth);
            return tree;
        }
    } // namespace

    MailImportScanPlanResult planMailImportSources(const MailImportOperationRecord& operation)
    {
        MailImportScanPlan plan;
        std::size_t ordinal = 0;
        std::set<QString> mailboxPaths;

        if (operation.recreateHierarchy && operation.sourcePaths.size() != 1)
        {
            return invalidImport(
                i18n("Folder hierarchy import requires exactly one source directory."));
        }

        for (const auto& sourceRoot : operation.sourcePaths)
        {
            bool sawEml = false;
            bool sawMbox = false;
            const QFileInfo sourceInfo{sourceRoot};
            if (!sourceInfo.exists() || !sourceInfo.isReadable() || sourceInfo.isSymLink())
            {
                return invalidImport(
                    i18n("Import source is unavailable or is a symbolic link: %1", sourceRoot));
            }

            std::vector<QString> paths;
            std::vector<QString> sourceDirectories;
            std::optional<JavelinExportMetadata> exportMetadata;
            QString rootDirectory;
            if (sourceInfo.isDir())
            {
                rootDirectory = sourceInfo.absoluteFilePath();
                auto metadataResult = javelinExportMetadata(rootDirectory);
                if (const auto* error = std::get_if<OperationError>(&metadataResult))
                    return *error;
                exportMetadata =
                    std::get<std::optional<JavelinExportMetadata>>(std::move(metadataResult));
                auto enumerated = enumerateImportTree(rootDirectory);
                if (const auto* error = std::get_if<OperationError>(&enumerated))
                    return *error;
                auto tree = std::get<EnumeratedImportTree>(std::move(enumerated));
                paths = std::move(tree.files);
                sourceDirectories = std::move(tree.directories);
            }
            else if (sourceInfo.isFile())
            {
                if (operation.recreateHierarchy)
                    return invalidImport(i18n("Hierarchy recreation requires a directory source."));
                paths.push_back(sourceInfo.absoluteFilePath());
            }
            else
            {
                return invalidImport(
                    i18n("Import source is not a regular file or directory: %1", sourceRoot));
            }

            for (const auto& path : paths)
            {
                const auto detected = detectMailImportFile(path);
                if (const auto* error = std::get_if<OperationError>(&detected))
                    return *error;
                const auto kind = std::get<MailImportFileKind>(detected);
                if (exportMetadata.has_value())
                {
                    const bool expectedMbox = exportMetadata->format == JavelinExportFormat::MboxRd;
                    if ((kind == MailImportFileKind::Mbox) != expectedMbox)
                    {
                        return invalidImport(
                            i18n("The Javelin export contents do not match its recorded format."));
                    }
                }
                sawEml = sawEml || kind == MailImportFileKind::Eml;
                sawMbox = sawMbox || kind == MailImportFileKind::Mbox;
                if (sourceInfo.isDir() && sawEml && sawMbox)
                    return invalidImport(
                        i18n("A directory import cannot mix EML and mbox layouts."));

                const auto fingerprintResult = mailImportSourceFingerprint(path);
                if (const auto* error = std::get_if<OperationError>(&fingerprintResult))
                    return *error;
                const auto fingerprint = std::get<MailImportSourceFingerprint>(fingerprintResult);
                const auto relative =
                    sourceInfo.isDir()
                        ? cleanedRelativePath(QDir{rootDirectory}.relativeFilePath(path))
                        : QFileInfo{path}.fileName();
                QString destinationRelative;
                if (operation.recreateHierarchy)
                {
                    destinationRelative =
                        kind == MailImportFileKind::Mbox
                            ? cleanedRelativePath(withoutMailboxExtension(relative))
                            : cleanedRelativePath(QFileInfo{relative}.path());
                    if (!destinationRelative.isEmpty())
                    {
                        for (const auto& prefix : pathPrefixes(destinationRelative))
                            mailboxPaths.insert(prefix);
                    }
                    else if (!operation.mailboxId.has_value())
                    {
                        return invalidImport(i18n("Loose messages at the root of a hierarchy "
                                                  "import need a target mailbox."));
                    }
                }

                const auto resolvedMailboxId = operation.recreateHierarchy
                                                   ? std::optional<std::string>{}
                                                   : operation.mailboxId;
                if (kind == MailImportFileKind::Eml)
                {
                    plan.items.push_back({
                        .itemId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
                        .ordinal = ordinal++,
                        .sourcePath = path,
                        .sourceRelativePath = relative,
                        .sourceKind = kind,
                        .contentOffset = std::nullopt,
                        .contentEnd = std::nullopt,
                        .decodedSize = fingerprint.size,
                        .sourceFingerprint = fingerprint,
                        .receivedAt = std::nullopt,
                        .destinationRelativePath = operation.recreateHierarchy
                                                       ? std::optional<QString>{destinationRelative}
                                                       : std::nullopt,
                        .resolvedMailboxId = resolvedMailboxId,
                    });
                }
                else
                {
                    const auto scanResult = scanMailImportMbox(path);
                    if (const auto* error = std::get_if<OperationError>(&scanResult))
                        return *error;
                    const auto& scan = std::get<MailImportMboxScan>(scanResult);
                    for (const auto& record : scan.records)
                    {
                        plan.items.push_back({
                            .itemId =
                                QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
                            .ordinal = ordinal++,
                            .sourcePath = path,
                            .sourceRelativePath = relative,
                            .sourceKind = kind,
                            .contentOffset = record.contentOffset,
                            .contentEnd = record.contentEnd,
                            .decodedSize = record.decodedSize,
                            .sourceFingerprint = scan.fingerprint,
                            .receivedAt = record.receivedAt,
                            .destinationRelativePath =
                                operation.recreateHierarchy
                                    ? std::optional<QString>{destinationRelative}
                                    : std::nullopt,
                            .resolvedMailboxId = resolvedMailboxId,
                        });
                    }
                }
            }

            if (operation.recreateHierarchy && sourceInfo.isDir())
            {
                const bool directoryMailboxes =
                    exportMetadata.has_value() ? exportMetadata->format == JavelinExportFormat::Eml
                                               : sawEml && !sawMbox;
                if (directoryMailboxes)
                {
                    for (const auto& directory : sourceDirectories)
                    {
                        const auto relative =
                            cleanedRelativePath(QDir{rootDirectory}.relativeFilePath(directory));
                        if (!relative.isEmpty())
                            mailboxPaths.insert(relative);
                    }
                }
            }
        }

        std::size_t mailboxOrdinal = 0;
        std::vector<QString> orderedMailboxes{mailboxPaths.begin(), mailboxPaths.end()};
        std::ranges::sort(orderedMailboxes,
                          [](const QString& left, const QString& right)
                          {
                              const auto leftDepth = pathDepth(left);
                              const auto rightDepth = pathDepth(right);
                              return leftDepth == rightDepth ? left < right
                                                             : leftDepth < rightDepth;
                          });
        for (const auto& relativePath : orderedMailboxes)
        {
            const QFileInfo info{relativePath};
            const auto parent = cleanedRelativePath(info.path());
            plan.mailboxes.push_back({
                .ordinal = mailboxOrdinal++,
                .relativePath = relativePath,
                .parentRelativePath =
                    parent.isEmpty() ? std::nullopt : std::optional<QString>{parent},
                .displayName = info.fileName(),
            });
        }
        return plan;
    }
} // namespace javelin::app
