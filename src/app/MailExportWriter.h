#pragma once

#include <QFileDevice>
#include <QString>

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace javelin::app
{
    struct MailExportFileWriteResult
    {
        QString path;
        QFileDevice::FileError fileError = QFileDevice::NoError;
        QString error;
    };

    struct MailExportFileHashResult
    {
        QString sha256;
        QString error;
    };

    [[nodiscard]] MailExportFileHashResult hashFileSha256(const QString& path);
    [[nodiscard]] MailExportFileWriteResult copyEmlFile(const QString& sourcePath,
                                                        const QString& targetPath);
    [[nodiscard]] MailExportFileWriteResult
    appendMboxRdRecord(const QString& sourcePath, const QString& partPath,
                       const std::optional<std::string>& senderEmail, std::string_view receivedAt);
} // namespace javelin::app
