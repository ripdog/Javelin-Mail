#include "gui/shell/MessageFileUtils.h"

#include <QDir>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QSaveFile>

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

    QString suggestedFileName(const javelin::jmap::AttachmentDownload& download)
    {
        QString fileName =
            QString::fromStdString(download.name.value_or("attachment-" + download.partId));
        fileName = sanitizedFileName(
            fileName, QStringLiteral("attachment-%1").arg(QString::fromStdString(download.partId)));

        if (!fileName.contains(QLatin1Char('.')))
        {
            const QMimeDatabase mimeDatabase;
            const auto mimeType =
                mimeDatabase.mimeTypeForName(QString::fromStdString(download.mediaType));
            const auto suffix = mimeType.preferredSuffix();
            if (!suffix.isEmpty())
            {
                fileName += QStringLiteral(".") + suffix;
            }
        }

        return fileName;
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
            if (!isEmbeddedInline(attachment) && attachment.blobId.has_value())
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

    QString tempAttachmentPath(QTemporaryDir& directory,
                               const javelin::jmap::AttachmentDownload& download)
    {
        return directory.filePath(QStringLiteral("%1-%2").arg(
            QString::fromStdString(download.emailId), suggestedFileName(download)));
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

    QString tempMessageSourcePath(QTemporaryDir& directory,
                                  const javelin::jmap::MessageSourceDownload& download)
    {
        return directory.filePath(QStringLiteral("%1-%2").arg(
            QString::fromStdString(download.emailId), suggestedSourceFileName(download)));
    }

} // namespace javelin::gui::shell
