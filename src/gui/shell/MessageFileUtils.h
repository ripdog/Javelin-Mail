#pragma once

#include "app/MessageContentApplicationPorts.h"
#include "jmap/cache/MessageViewReader.h"

#include <QCoroTask>

#include <QByteArray>
#include <QString>

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
