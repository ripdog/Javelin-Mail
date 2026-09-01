#include "gui/shell/MessageFileUtils.h"

#include <KLocalizedString>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <algorithm>
#include <cctype>

namespace javelin::gui::shell
{
    namespace
    {

        [[nodiscard]] QString sanitizedFileName(QString name, const QString& fallback)
        {
            name = name.trimmed();
            if (name.isEmpty())
            {
                name = fallback;
            }

            static const QRegularExpression invalidPattern{
                QStringLiteral(R"([\\/:*?"<>|\x00-\x1F])")};
            name.replace(invalidPattern, QStringLiteral("_"));
            return name.isEmpty() ? fallback : name;
        }

        [[nodiscard]] QString attachmentFileName(const std::optional<std::string>& name,
                                                 const std::string& partId,
                                                 const std::string& mediaType)
        {
            QString fileName =
                name.has_value()
                    ? QString::fromStdString(*name)
                    : QStringLiteral("attachment-%1").arg(QString::fromStdString(partId));
            fileName = sanitizedFileName(
                fileName, QStringLiteral("attachment-%1").arg(QString::fromStdString(partId)));

            if (!fileName.contains(QLatin1Char('.')))
            {
                const QMimeDatabase mimeDatabase;
                const auto mimeType =
                    mimeDatabase.mimeTypeForName(QString::fromStdString(mediaType));
                const auto suffix = mimeType.preferredSuffix();
                if (!suffix.isEmpty())
                    fileName += QStringLiteral(".") + suffix;
            }
            return fileName;
        }

        [[nodiscard]] bool
        isEmbeddedInline(const javelin::jmap::cache::MessageAttachment& attachment)
        {
            return attachment.cid.has_value() && attachment.disposition.has_value() &&
                   std::ranges::equal(*attachment.disposition, std::string_view{"inline"},
                                      [](const char left, const char right)
                                      {
                                          return std::tolower(static_cast<unsigned char>(left)) ==
                                                 std::tolower(static_cast<unsigned char>(right));
                                      });
        }

    } // namespace

    bool isDownloadableAttachment(const javelin::jmap::cache::MessageAttachment& attachment)
    {
        return !isEmbeddedInline(attachment) && attachment.blobId.has_value();
    }

    QString suggestedFileName(const javelin::jmap::AttachmentDownload& download)
    {
        return attachmentFileName(download.name, download.partId, download.mediaType);
    }

    QString suggestedFileName(const javelin::jmap::cache::MessageAttachment& attachment)
    {
        return attachmentFileName(attachment.name, attachment.partId, attachment.mediaType);
    }

    QString uniqueFilePath(const QString& directoryPath, const QString& fileName)
    {
        const QDir directory{directoryPath};
        const QFileInfo original{fileName};
        const auto completeSuffix = original.completeSuffix();
        const auto baseName = completeSuffix.isEmpty() ? fileName : original.completeBaseName();
        const auto suffix =
            completeSuffix.isEmpty() ? QString{} : QStringLiteral(".") + completeSuffix;

        auto candidate = directory.filePath(fileName);
        for (int index = 1; QFileInfo::exists(candidate); ++index)
        {
            candidate = directory.filePath(
                QStringLiteral("%1 (%2)%3").arg(baseName).arg(index).arg(suffix));
        }

        return candidate;
    }

    FileWriteResult writePayloadToPath(const QString& path, const QByteArray& payload)
    {
        QSaveFile file{path};
        if (!file.open(QIODevice::WriteOnly))
        {
            return FileWriteResult{
                .path = path,
                .errorMessage = file.errorString(),
            };
        }

        if (file.write(payload) != payload.size())
        {
            return FileWriteResult{
                .path = path,
                .errorMessage = file.errorString(),
            };
        }

        if (!file.commit())
        {
            return FileWriteResult{
                .path = path,
                .errorMessage = file.errorString(),
            };
        }

        return FileWriteResult{.path = path, .errorMessage = {}};
    }

    FileWriteResult writePayloadToTemporaryFile(const QString& suggestedFileName,
                                                const QByteArray& payload)
    {
        const auto fileTemplate = QDir{QDir::tempPath()}.filePath(
            QStringLiteral("javelin-XXXXXX-%1").arg(suggestedFileName));
        QTemporaryFile file{fileTemplate};
        if (!file.open())
            return FileWriteResult{.path = {}, .errorMessage = file.errorString()};

        if (file.write(payload) != payload.size())
            return FileWriteResult{.path = file.fileName(), .errorMessage = file.errorString()};
        if (!file.flush())
            return FileWriteResult{.path = file.fileName(), .errorMessage = file.errorString()};

        file.setAutoRemove(false);
        const auto path = file.fileName();
        file.close();
        return FileWriteResult{.path = path, .errorMessage = {}};
    }

    QString defaultExternalDragRootPath()
    {
        return QDir{QStandardPaths::writableLocation(QStandardPaths::CacheLocation)}.filePath(
            QStringLiteral("drag-out"));
    }

    void cleanupExpiredExternalDragDirectories(const QString& rootPath,
                                               const qint64 nowMilliseconds,
                                               const qint64 retentionMilliseconds)
    {
        if (retentionMilliseconds < 0)
            return;

        QDir root{rootPath};
        if (!root.exists())
            return;

        const auto directories = root.entryInfoList({QStringLiteral("drag-*")},
                                                    QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const auto& directory : directories)
        {
            const auto name = directory.fileName();
            const auto separator = name.indexOf(QLatin1Char('-'), 5);
            if (separator <= 5)
                continue;
            bool ok = false;
            const auto createdMilliseconds = name.mid(5, separator - 5).toLongLong(&ok);
            if (!ok || createdMilliseconds > nowMilliseconds ||
                nowMilliseconds - createdMilliseconds < retentionMilliseconds)
                continue;
            QDir{directory.absoluteFilePath()}.removeRecursively();
        }
    }

    ExternalDragDirectoryResult createExternalDragDirectory(const QString& rootPath,
                                                            qint64 nowMilliseconds)
    {
        if (nowMilliseconds < 0)
            nowMilliseconds = QDateTime::currentMSecsSinceEpoch();

        if (!QDir{}.mkpath(rootPath))
        {
            return ExternalDragDirectoryResult{
                .path = {},
                .errorMessage = i18n("Could not create the drag-out cache directory."),
            };
        }
        cleanupExpiredExternalDragDirectories(rootPath, nowMilliseconds);

        QTemporaryDir directory{
            QDir{rootPath}.filePath(QStringLiteral("drag-%1-XXXXXX").arg(nowMilliseconds))};
        if (!directory.isValid())
        {
            return ExternalDragDirectoryResult{
                .path = {},
                .errorMessage = directory.errorString(),
            };
        }
        directory.setAutoRemove(false);
        const auto path = directory.path();
        static_cast<void>(QFile::setPermissions(
            path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        return ExternalDragDirectoryResult{.path = path, .errorMessage = {}};
    }

    QList<QUrl> externalDragFileUrls(const QString& directoryPath)
    {
        const auto files = QDir{directoryPath}.entryInfoList(QDir::Files | QDir::NoDotAndDotDot,
                                                             QDir::Name | QDir::IgnoreCase);
        QList<QUrl> urls;
        urls.reserve(files.size());
        for (const auto& file : files)
            urls.push_back(QUrl::fromLocalFile(file.absoluteFilePath()));
        return urls;
    }

    BatchWriteResult
    writePayloadBatchToDirectory(const QString& directoryPath,
                                 const std::vector<DownloadedAttachmentFile>& files)
    {
        QDir directory{directoryPath};
        BatchWriteResult result;
        for (const auto& file : files)
        {
            const auto writeResult = writePayloadToPath(
                uniqueFilePath(directory.absolutePath(), file.fileName), file.payload);
            if (!writeResult.errorMessage.isEmpty())
            {
                return BatchWriteResult{
                    .savedCount = result.savedCount,
                    .errorMessage = writeResult.errorMessage,
                    .failedPath = writeResult.path,
                };
            }

            ++result.savedCount;
        }

        return result;
    }

    std::vector<javelin::jmap::cache::MessageAttachment>
    visibleDownloadableAttachments(const javelin::jmap::cache::MessageViewSnapshot& snapshot)
    {
        std::vector<javelin::jmap::cache::MessageAttachment> attachments;
        attachments.reserve(snapshot.attachments.size());
        for (const auto& attachment : snapshot.attachments)
        {
            if (isDownloadableAttachment(attachment))
            {
                attachments.push_back(attachment);
            }
        }

        return attachments;
    }

    QCoro::Task<SaveAllDownloadResult>
    downloadAttachments(javelin::app::MessageContentPort& contentPort, std::string accountId,
                        std::string emailId,
                        std::vector<javelin::jmap::cache::MessageAttachment> attachments)
    {
        SaveAllDownloadResult result;
        result.files.reserve(attachments.size());

        for (const auto& attachment : attachments)
        {
            const auto downloadResult =
                co_await contentPort.requestAttachment(accountId, emailId, attachment.partId);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&downloadResult))
            {
                co_return SaveAllDownloadResult{
                    .files = {},
                    .errorMessage = error->message,
                };
            }

            const auto& download = std::get<javelin::jmap::AttachmentDownload>(downloadResult);
            result.files.push_back(DownloadedAttachmentFile{
                .fileName = suggestedFileName(download),
                .payload = download.payload,
            });
        }

        co_return result;
    }

    QString suggestedSourceFileName(const javelin::jmap::MessageSourceDownload& download)
    {
        QString baseName = download.subject.has_value()
                               ? sanitizedFileName(QString::fromStdString(*download.subject),
                                                   QStringLiteral("message"))
                               : QStringLiteral("message");
        if (!baseName.endsWith(QStringLiteral(".eml"), Qt::CaseInsensitive))
        {
            baseName += QStringLiteral(".eml");
        }

        return baseName;
    }

} // namespace javelin::gui::shell
