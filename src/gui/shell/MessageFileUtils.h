#pragma once

#include "app/LongPollCoordinator.h"
#include "jmap/cache/MessageViewService.h"

#include <QCoroTask>

#include <QByteArray>
#include <QString>
#include <QTemporaryDir>

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

    [[nodiscard]] QString suggestedFileName(const javelin::jmap::AttachmentDownload& download);
    [[nodiscard]] QString uniqueFilePath(const QString& directoryPath, const QString& fileName);
    [[nodiscard]] FileWriteResult writePayloadToPath(const QString& path,
                                                     const QByteArray& payload);
    [[nodiscard]] BatchWriteResult
    writePayloadBatchToDirectory(const QString& directoryPath,
                                 const std::vector<DownloadedAttachmentFile>& files);
    [[nodiscard]] std::vector<javelin::jmap::cache::MessageAttachment>
    visibleDownloadableAttachments(const javelin::jmap::cache::MessageViewSnapshot& snapshot);
    [[nodiscard]] QCoro::Task<SaveAllDownloadResult>
    downloadAttachments(javelin::app::LongPollCoordinator& mailService, std::string accountId,
                        std::string emailId,
                        std::vector<javelin::jmap::cache::MessageAttachment> attachments);
    [[nodiscard]] QString tempAttachmentPath(QTemporaryDir& directory,
                                             const javelin::jmap::AttachmentDownload& download);
    [[nodiscard]] QString
    suggestedSourceFileName(const javelin::jmap::MessageSourceDownload& download);
    [[nodiscard]] QString
    tempMessageSourcePath(QTemporaryDir& directory,
                          const javelin::jmap::MessageSourceDownload& download);

} // namespace javelin::gui::shell
