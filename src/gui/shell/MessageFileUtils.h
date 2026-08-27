#pragma once

#include "app/MessageContentApplicationPorts.h"
#include "jmap/cache/MessageViewReader.h"

#include <QCoroTask>

#include <QByteArray>
#include <QList>
#include <QString>
#include <QUrl>

#include <string>
#include <vector>

namespace javelin::gui::shell
{

    struct FileWriteResult
    {
        QString path;
        QString errorMessage;
    };

    struct DownloadedAttachmentFile
    {
        QString fileName;
        QByteArray payload;
    };

    struct SaveAllDownloadResult
    {
        std::vector<DownloadedAttachmentFile> files;
        QString errorMessage;
    };

    struct BatchWriteResult
    {
        int savedCount = 0;
        QString errorMessage;
        QString failedPath;
    };

    struct ExternalDragDirectoryResult
    {
        QString path;
        QString errorMessage;
    };

    inline constexpr qint64 externalDragRetentionMilliseconds = 24LL * 60LL * 60LL * 1000LL;

    [[nodiscard]] bool
    isDownloadableAttachment(const javelin::jmap::cache::MessageAttachment& attachment);
    [[nodiscard]] QString suggestedFileName(const javelin::jmap::AttachmentDownload& download);
    [[nodiscard]] QString
    suggestedFileName(const javelin::jmap::cache::MessageAttachment& attachment);
    [[nodiscard]] QString uniqueFilePath(const QString& directoryPath, const QString& fileName);
    [[nodiscard]] FileWriteResult writePayloadToPath(const QString& path,
                                                     const QByteArray& payload);
    [[nodiscard]] FileWriteResult writePayloadToTemporaryFile(const QString& suggestedFileName,
                                                              const QByteArray& payload);
    [[nodiscard]] QString defaultExternalDragRootPath();
    void cleanupExpiredExternalDragDirectories(
        const QString& rootPath, qint64 nowMilliseconds,
        qint64 retentionMilliseconds = externalDragRetentionMilliseconds);
    [[nodiscard]] ExternalDragDirectoryResult
    createExternalDragDirectory(const QString& rootPath, qint64 nowMilliseconds = -1);
    [[nodiscard]] QList<QUrl> externalDragFileUrls(const QString& directoryPath);
    [[nodiscard]] BatchWriteResult
    writePayloadBatchToDirectory(const QString& directoryPath,
                                 const std::vector<DownloadedAttachmentFile>& files);
    [[nodiscard]] std::vector<javelin::jmap::cache::MessageAttachment>
    visibleDownloadableAttachments(const javelin::jmap::cache::MessageViewSnapshot& snapshot);
    [[nodiscard]] QCoro::Task<SaveAllDownloadResult>
    downloadAttachments(javelin::app::MessageContentPort& contentPort, std::string accountId,
                        std::string emailId,
                        std::vector<javelin::jmap::cache::MessageAttachment> attachments);
    [[nodiscard]] QString
    suggestedSourceFileName(const javelin::jmap::MessageSourceDownload& download);

} // namespace javelin::gui::shell
