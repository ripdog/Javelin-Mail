#include "app/MailImportSource.h"

#include <KLocalizedString>

#include <QByteArray>
#include <QDateTime>
#include <QFileInfo>
#include <QLocale>
#include <QTimeZone>

#include <algorithm>
#include <cstring>
#include <limits>

namespace javelin::app
{
    namespace
    {
        using javelin::jmap::OperationError;
        using javelin::jmap::OperationErrorCode;

        constexpr qint64 lineBufferSize = 8192;
        constexpr qsizetype prefixLimit = 512;

        struct BoundedLine
        {
            qint64 start = 0;
            qint64 end = 0;
            QByteArray prefix;
            bool endedWithNewline = false;
        };

        [[nodiscard]] OperationError localReadError(const QString& path, const QString& detail)
        {
            return {
                .code = OperationErrorCode::LocalStorageFailure,
                .message = i18n("Could not read mail import source %1: %2", path, detail),
            };
        }

        [[nodiscard]] OperationError invalidSource(const QString& path, const QString& detail)
        {
            return {
                .code = OperationErrorCode::InvalidUserInput,
                .message = i18n("Cannot import %1: %2", path, detail),
            };
        }

        [[nodiscard]] std::variant<BoundedLine, OperationError> readBoundedLine(QFile& file)
        {
            BoundedLine line{
                .start = file.pos(), .end = file.pos(), .prefix = {}, .endedWithNewline = false};
            char buffer[lineBufferSize];
            while (!file.atEnd())
            {
                const auto count = file.readLine(buffer, lineBufferSize);
                if (count < 0)
                    return localReadError(file.fileName(), file.errorString());
                if (count == 0)
                    break;
                if (line.prefix.size() < prefixLimit)
                {
                    const auto wanted = std::min<qsizetype>(prefixLimit - line.prefix.size(),
                                                            static_cast<qsizetype>(count));
                    line.prefix.append(buffer, wanted);
                }
                if (buffer[count - 1] == '\n')
                {
                    line.endedWithNewline = true;
                    break;
                }
            }
            line.end = file.pos();
            return line;
        }

        [[nodiscard]] bool isFourDigitYear(const QStringView value)
        {
            if (value.size() != 4)
                return false;
            return std::ranges::all_of(value,
                                       [](const QChar character) { return character.isDigit(); });
        }

        [[nodiscard]] bool plausibleMboxSeparator(const QByteArray& prefix)
        {
            QByteArrayView line{prefix};
            if (!line.startsWith("From "))
                return false;
            const auto senderEnd = line.indexOf(' ', 5);
            if (senderEnd <= 5)
                return false;
            const auto datePart = line.sliced(senderEnd + 1);
            if (datePart.indexOf(':') < 0)
                return false;
            int spaces = 0;
            bool hasDigit = false;
            for (const char character : datePart)
            {
                if (character == ' ' || character == '\t')
                    ++spaces;
                else if (character >= '0' && character <= '9')
                    hasDigit = true;
                if (character == '\n' || character == '\r')
                    break;
            }
            return hasDigit && spaces >= 3;
        }

        [[nodiscard]] bool isRfc5322FieldName(const QByteArrayView value)
        {
            if (value.isEmpty())
                return false;
            const auto first = static_cast<unsigned char>(value.front());
            if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') ||
                  (first >= '0' && first <= '9')))
                return false;
            return std::ranges::all_of(value,
                                       [](const char character)
                                       {
                                           const auto byte = static_cast<unsigned char>(character);
                                           return (byte >= 33 && byte <= 57) ||
                                                  (byte >= 59 && byte <= 126);
                                       });
        }

        [[nodiscard]] bool plausibleRfc5322Message(const QByteArray& prefix)
        {
            QByteArrayView input{prefix};
            qsizetype offset = 0;
            bool sawHeader = false;
            while (offset < input.size())
            {
                const auto newline = input.indexOf('\n', offset);
                const auto lineEnd = newline < 0 ? input.size() : newline;
                auto line = input.sliced(offset, lineEnd - offset);
                if (!line.isEmpty() && line.back() == '\r')
                    line = line.first(line.size() - 1);
                if (line.isEmpty())
                    return sawHeader;
                if (line.front() == ' ' || line.front() == '\t')
                {
                    if (!sawHeader)
                        return false;
                }
                else
                {
                    const auto colon = line.indexOf(':');
                    if (colon <= 0 || !isRfc5322FieldName(line.first(colon)))
                        return false;
                    sawHeader = true;
                }
                if (newline < 0)
                    break;
                offset = newline + 1;
            }
            return sawHeader;
        }

        [[nodiscard]] std::optional<std::string> parseSeparatorDate(const QByteArray& prefix)
        {
            auto line = QString::fromLatin1(prefix).trimmed();
            const auto parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (parts.size() < 7 || parts.front() != QStringLiteral("From"))
                return std::nullopt;

            qsizetype yearIndex = -1;
            for (qsizetype index = parts.size() - 1; index >= 2; --index)
            {
                if (isFourDigitYear(QStringView{parts[index]}))
                {
                    yearIndex = index;
                    break;
                }
            }
            if (yearIndex < 6)
                return std::nullopt;

            const QString normalized =
                QStringLiteral("%1 %2 %3 %4 %5")
                    .arg(parts[2], parts[3], parts[4], parts[5], parts[yearIndex]);
            auto parsed =
                QLocale::c().toDateTime(normalized, QStringLiteral("ddd MMM d HH:mm:ss yyyy"));
            if (!parsed.isValid())
                parsed =
                    QLocale::c().toDateTime(normalized, QStringLiteral("ddd MMM dd HH:mm:ss yyyy"));
            if (!parsed.isValid())
                return std::nullopt;
            parsed.setTimeZone(QTimeZone::UTC);
            return parsed.toString(Qt::ISODate).toStdString();
        }

        [[nodiscard]] bool lineNeedsMboxUnquote(const QByteArray& prefix)
        {
            qsizetype index = 0;
            while (index < prefix.size() && prefix[index] == '>')
                ++index;
            return index > 0 && prefix.size() >= index + 5 &&
                   QByteArrayView{prefix}.sliced(index, 5) == QByteArrayView{"From ", 5};
        }

        [[nodiscard]] std::pair<qint64, qint64> trimmedRecordEnd(QFile& file, const qint64 boundary)
        {
            if (boundary <= 1)
                return {boundary, 0};
            const auto saved = file.pos();
            const qint64 start = std::max<qint64>(0, boundary - 4);
            if (!file.seek(start))
                return {boundary, 0};
            const auto suffix = file.read(boundary - start);
            static_cast<void>(file.seek(saved));
            if (suffix.endsWith(QByteArrayLiteral("\r\n\r\n")))
                return {boundary - 2, 2};
            if (suffix.endsWith(QByteArrayLiteral("\n\n")))
                return {boundary - 1, 1};
            return {boundary, 0};
        }

        [[nodiscard]] std::variant<MailImportSourceFingerprint, OperationError>
        fingerprintFor(const QString& path)
        {
            const QFileInfo info{path};
            if (!info.exists() || !info.isFile() || !info.isReadable())
                return invalidSource(path, i18n("the file is not readable"));
            const auto size = info.size();
            if (size < 0)
                return localReadError(path, i18n("the file size is unavailable"));
            auto canonical = info.canonicalFilePath();
            if (canonical.isEmpty())
                canonical = info.absoluteFilePath();
            return MailImportSourceFingerprint{
                .canonicalPath = std::move(canonical),
                .size = static_cast<std::uint64_t>(size),
                .lastModifiedMs = info.lastModified().toMSecsSinceEpoch(),
            };
        }
    } // namespace

    MailImportFileDetectionResult detectMailImportFile(const QString& path)
    {
        const QFileInfo info{path};
        if (!info.exists() || !info.isFile() || !info.isReadable())
            return invalidSource(path, i18n("the file is not readable"));
        if (info.size() == 0)
        {
            const auto suffix = info.suffix().toCaseFolded();
            if (suffix == QStringLiteral("mbox") || suffix == QStringLiteral("mbx"))
                return MailImportFileKind::Mbox;
            return invalidSource(path, i18n("the file is empty"));
        }

        QFile file{path};
        if (!file.open(QIODevice::ReadOnly))
            return localReadError(path, file.errorString());
        const auto prefix = file.peek(prefixLimit);
        if (prefix.contains('\0'))
            return invalidSource(path, i18n("the file appears to contain binary data"));
        if (plausibleMboxSeparator(prefix))
            return MailImportFileKind::Mbox;
        if (!plausibleRfc5322Message(prefix))
            return invalidSource(path,
                                 i18n("the file does not begin with RFC 5322 message headers"));
        return MailImportFileKind::Eml;
    }

    MailImportMboxScanResult scanMailImportMbox(const QString& path)
    {
        auto fingerprintResult = fingerprintFor(path);
        if (const auto* error = std::get_if<OperationError>(&fingerprintResult))
            return *error;
        auto fingerprint = std::get<MailImportSourceFingerprint>(std::move(fingerprintResult));
        if (fingerprint.size == 0)
            return MailImportMboxScan{.fingerprint = std::move(fingerprint), .records = {}};

        QFile file{path};
        if (!file.open(QIODevice::ReadOnly))
            return localReadError(path, file.errorString());

        auto firstResult = readBoundedLine(file);
        if (const auto* error = std::get_if<OperationError>(&firstResult))
            return *error;
        const auto first = std::get<BoundedLine>(std::move(firstResult));
        if (!plausibleMboxSeparator(first.prefix))
            return invalidSource(path,
                                 i18n("the file does not begin with an mbox From_ separator"));

        std::vector<MailImportMboxRecord> records;
        auto currentOffset = static_cast<std::uint64_t>(first.end);
        auto currentReceivedAt = parseSeparatorDate(first.prefix);
        std::uint64_t decodedBytes = 0;

        while (!file.atEnd())
        {
            auto lineResult = readBoundedLine(file);
            if (const auto* error = std::get_if<OperationError>(&lineResult))
                return *error;
            const auto line = std::get<BoundedLine>(std::move(lineResult));
            if (line.end == line.start)
                break;

            if (plausibleMboxSeparator(line.prefix))
            {
                const auto [contentEnd, trimmedBytes] = trimmedRecordEnd(file, line.start);
                if (contentEnd < static_cast<qint64>(currentOffset) ||
                    decodedBytes < static_cast<std::uint64_t>(trimmedBytes))
                    return invalidSource(path, i18n("the mbox record framing is inconsistent"));
                records.push_back({
                    .ordinal = records.size(),
                    .contentOffset = currentOffset,
                    .contentEnd = static_cast<std::uint64_t>(contentEnd),
                    .decodedSize = decodedBytes - static_cast<std::uint64_t>(trimmedBytes),
                    .receivedAt = std::move(currentReceivedAt),
                });
                currentOffset = static_cast<std::uint64_t>(line.end);
                currentReceivedAt = parseSeparatorDate(line.prefix);
                decodedBytes = 0;
                continue;
            }

            auto bytes = static_cast<std::uint64_t>(line.end - line.start);
            if (lineNeedsMboxUnquote(line.prefix))
            {
                if (bytes == 0)
                    return invalidSource(path, i18n("the mbox quoting is inconsistent"));
                --bytes;
            }
            if (std::numeric_limits<std::uint64_t>::max() - decodedBytes < bytes)
                return invalidSource(path, i18n("the mbox message is too large"));
            decodedBytes += bytes;
        }

        const auto boundary = file.size();
        const auto [contentEnd, trimmedBytes] = trimmedRecordEnd(file, boundary);
        if (contentEnd < static_cast<qint64>(currentOffset) ||
            decodedBytes < static_cast<std::uint64_t>(trimmedBytes))
            return invalidSource(path, i18n("the final mbox record framing is inconsistent"));
        records.push_back({
            .ordinal = records.size(),
            .contentOffset = currentOffset,
            .contentEnd = static_cast<std::uint64_t>(contentEnd),
            .decodedSize = decodedBytes - static_cast<std::uint64_t>(trimmedBytes),
            .receivedAt = std::move(currentReceivedAt),
        });

        return MailImportMboxScan{.fingerprint = std::move(fingerprint),
                                  .records = std::move(records)};
    }

    std::variant<MailImportSourceFingerprint, OperationError>
    mailImportSourceFingerprint(const QString& path)
    {
        return fingerprintFor(path);
    }

    MailImportMboxRecordDevice::MailImportMboxRecordDevice(QString path,
                                                           MailImportMboxRecord record,
                                                           QObject* parent)
        : QIODevice(parent), m_path(std::move(path)), m_record(std::move(record)), m_file(m_path)
    {
    }

    bool MailImportMboxRecordDevice::openReadOnly()
    {
        if (isOpen())
            close();
        if (m_file.isOpen())
            m_file.close();
        m_file.setFileName(m_path);
        if (!m_file.open(QIODevice::ReadOnly) ||
            !m_file.seek(static_cast<qint64>(m_record.contentOffset)))
            return false;
        m_decodedPosition = 0;
        m_atLineStart = true;
        return QIODevice::open(QIODevice::ReadOnly);
    }

    bool MailImportMboxRecordDevice::isSequential() const
    {
        return false;
    }

    qint64 MailImportMboxRecordDevice::size() const
    {
        return static_cast<qint64>(m_record.decodedSize);
    }

    qint64 MailImportMboxRecordDevice::pos() const
    {
        return m_decodedPosition;
    }

    bool MailImportMboxRecordDevice::seek(const qint64 position)
    {
        if (position != 0 || !m_file.isOpen() ||
            !m_file.seek(static_cast<qint64>(m_record.contentOffset)))
            return false;
        m_decodedPosition = 0;
        m_atLineStart = true;
        return QIODevice::seek(0);
    }

    bool MailImportMboxRecordDevice::skipMboxQuoteAtLineStart()
    {
        if (!m_atLineStart || static_cast<std::uint64_t>(m_file.pos()) >= m_record.contentEnd)
            return false;
        const auto start = m_file.pos();
        std::size_t greaterThanCount = 0;
        char character = 0;
        while (static_cast<std::uint64_t>(m_file.pos()) < m_record.contentEnd)
        {
            if (m_file.read(&character, 1) != 1)
                break;
            if (character != '>')
                break;
            ++greaterThanCount;
        }
        bool quoted = false;
        if (greaterThanCount > 0 && character == 'F')
        {
            char suffix[4]{};
            if (m_file.read(suffix, 4) == 4 && std::memcmp(suffix, "rom ", 4) == 0)
                quoted = true;
        }
        static_cast<void>(m_file.seek(start + (quoted ? 1 : 0)));
        return quoted;
    }

    qint64 MailImportMboxRecordDevice::readData(char* data, const qint64 maxSize)
    {
        if (maxSize <= 0 || m_decodedPosition >= size())
            return 0;
        qint64 written = 0;
        const auto target = std::min<qint64>(maxSize, size() - m_decodedPosition);
        while (written < target && static_cast<std::uint64_t>(m_file.pos()) < m_record.contentEnd)
        {
            if (m_atLineStart)
                static_cast<void>(skipMboxQuoteAtLineStart());
            const auto rawRemaining = static_cast<qint64>(m_record.contentEnd) - m_file.pos();
            const auto wanted = std::min<qint64>(target - written, rawRemaining);
            if (wanted <= 0)
                break;
            const auto chunk = m_file.readLine(wanted + 1);
            if (chunk.isEmpty())
            {
                if (m_file.error() != QFileDevice::NoError)
                    return written == 0 ? -1 : written;
                break;
            }
            std::memcpy(data + written, chunk.constData(), static_cast<std::size_t>(chunk.size()));
            written += chunk.size();
            m_decodedPosition += chunk.size();
            m_atLineStart = chunk.endsWith('\n');
        }
        return written;
    }

    qint64 MailImportMboxRecordDevice::writeData(const char*, const qint64)
    {
        return -1;
    }
} // namespace javelin::app
