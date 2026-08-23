#pragma once

#include "jmap/OperationError.h"

#include <QFile>
#include <QIODevice>
#include <QString>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace javelin::app
{
    enum class MailImportFileKind
    {
        Eml,
        Mbox,
    };

    struct MailImportSourceFingerprint
    {
        QString canonicalPath;
        std::uint64_t size = 0;
        qint64 lastModifiedMs = 0;

        friend bool operator==(const MailImportSourceFingerprint&,
                               const MailImportSourceFingerprint&) = default;
    };

    struct MailImportMboxRecord
    {
        std::size_t ordinal = 0;
        std::uint64_t contentOffset = 0;
        std::uint64_t contentEnd = 0;
        std::uint64_t decodedSize = 0;
        std::optional<std::string> receivedAt;
    };

    struct MailImportMboxScan
    {
        MailImportSourceFingerprint fingerprint;
        std::vector<MailImportMboxRecord> records;
    };

    using MailImportFileDetectionResult =
        std::variant<MailImportFileKind, javelin::jmap::OperationError>;
    using MailImportMboxScanResult =
        std::variant<MailImportMboxScan, javelin::jmap::OperationError>;

    [[nodiscard]] MailImportFileDetectionResult detectMailImportFile(const QString& path);
    [[nodiscard]] MailImportMboxScanResult scanMailImportMbox(const QString& path);
    [[nodiscard]] std::variant<MailImportSourceFingerprint, javelin::jmap::OperationError>
    mailImportSourceFingerprint(const QString& path);

    class MailImportMboxRecordDevice final : public QIODevice
    {
      public:
        MailImportMboxRecordDevice(QString path, MailImportMboxRecord record,
                                   QObject* parent = nullptr);

        [[nodiscard]] bool openReadOnly();
        [[nodiscard]] bool isSequential() const override;
        [[nodiscard]] qint64 size() const override;
        [[nodiscard]] qint64 pos() const override;
        [[nodiscard]] bool seek(qint64 position) override;

      protected:
        [[nodiscard]] qint64 readData(char* data, qint64 maxSize) override;
        [[nodiscard]] qint64 writeData(const char* data, qint64 maxSize) override;

      private:
        [[nodiscard]] bool skipMboxQuoteAtLineStart();

        QString m_path;
        MailImportMboxRecord m_record;
        QFile m_file;
        qint64 m_decodedPosition = 0;
        bool m_atLineStart = true;
    };
} // namespace javelin::app
