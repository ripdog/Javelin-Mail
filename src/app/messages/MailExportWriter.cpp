#include "app/MailExportWriter.h"

#include <KLocalizedString>

#include <QByteArray>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QLocale>
#include <QSaveFile>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace javelin::app
{
    namespace
    {
        [[nodiscard]] bool writeAll(QFile& file, const QByteArrayView bytes)
        {
            qsizetype offset = 0;
            while (offset < bytes.size())
            {
                const auto written = file.write(bytes.data() + offset, bytes.size() - offset);
                if (written <= 0)
                    return false;
                offset += written;
            }
            return true;
        }

        [[nodiscard]] bool lineNeedsMboxEscape(QFile& source, const qint64 lineStart)
        {
            if (!source.seek(lineStart))
                return false;
            qint64 leadingGreaterThan = 0;
            QByteArray chunk;
            chunk.resize(4096);
            while (true)
            {
                const auto count = source.read(chunk.data(), chunk.size());
                if (count <= 0)
                {
                    static_cast<void>(source.seek(lineStart));
                    return false;
                }
                for (qint64 index = 0; index < count; ++index)
                {
                    const char character = chunk[static_cast<qsizetype>(index)];
                    if (character == '>')
                    {
                        ++leadingGreaterThan;
                        continue;
                    }
                    if (character == '\n' || character == '\r')
                    {
                        static_cast<void>(source.seek(lineStart));
                        return false;
                    }
                    const auto prefixPosition = lineStart + leadingGreaterThan;
                    if (!source.seek(prefixPosition))
                    {
                        static_cast<void>(source.seek(lineStart));
                        return false;
                    }
                    const auto prefix = source.read(5);
                    static_cast<void>(source.seek(lineStart));
                    return prefix == QByteArrayLiteral("From ");
                }
            }
        }
    } // namespace

    MailExportFileHashResult hashFileSha256(const QString& path)
    {
        QFile file{path};
        if (!file.open(QIODevice::ReadOnly))
            return {.sha256 = {}, .error = file.errorString()};

        QCryptographicHash hash{QCryptographicHash::Sha256};
        QByteArray buffer;
        buffer.resize(1024 * 1024);
        while (true)
        {
            const auto count = file.read(buffer.data(), buffer.size());
            if (count < 0)
                return {.sha256 = {}, .error = file.errorString()};
            if (count == 0)
                break;
            hash.addData(QByteArrayView{buffer.constData(), count});
        }
        return {.sha256 = QString::fromLatin1(hash.result().toHex()), .error = {}};
    }

    MailExportFileWriteResult copyEmlFile(const QString& sourcePath, const QString& targetPath)
    {
        QFile source{sourcePath};
        if (!source.open(QIODevice::ReadOnly))
            return {.path = targetPath, .fileError = source.error(), .error = source.errorString()};
        QSaveFile target{targetPath};
        if (!target.open(QIODevice::WriteOnly))
            return {.path = targetPath, .fileError = target.error(), .error = target.errorString()};
        QByteArray buffer;
        buffer.resize(1024 * 1024);
        while (true)
        {
            const auto count = source.read(buffer.data(), buffer.size());
            if (count < 0)
                return {
                    .path = targetPath, .fileError = source.error(), .error = source.errorString()};
            if (count == 0)
                break;
            if (target.write(buffer.constData(), count) != count)
                return {
                    .path = targetPath, .fileError = target.error(), .error = target.errorString()};
        }
        if (!target.commit())
            return {.path = targetPath, .fileError = target.error(), .error = target.errorString()};
        return {.path = targetPath, .fileError = QFileDevice::NoError, .error = {}};
    }

    MailExportFileWriteResult appendMboxRdRecord(const QString& sourcePath, const QString& partPath,
                                                 const std::optional<std::string>& senderEmail,
                                                 const std::string_view receivedAt)
    {
        QFile source{sourcePath};
        if (!source.open(QIODevice::ReadOnly))
            return {.path = partPath, .fileError = source.error(), .error = source.errorString()};
        QFile target{partPath};
        if (!target.open(QIODevice::ReadWrite))
            return {.path = partPath, .fileError = target.error(), .error = target.errorString()};
        if (!target.seek(target.size()))
            return {.path = partPath, .fileError = target.error(), .error = target.errorString()};

        QString sender = QString::fromStdString(senderEmail.value_or("MAILER-DAEMON"));
        if (sender.isEmpty())
            sender = QStringLiteral("MAILER-DAEMON");
        sender.replace(QLatin1Char(' '), QLatin1Char('_'));
        auto received =
            QDateTime::fromString(QString::fromStdString(std::string{receivedAt}), Qt::ISODate);
        if (!received.isValid())
            received = QDateTime::currentDateTimeUtc();
        received = received.toUTC();
        const auto separator =
            QStringLiteral("From %1 %2\n")
                .arg(sender,
                     QLocale::c().toString(received, QStringLiteral("ddd MMM d HH:mm:ss yyyy")))
                .toUtf8();
        if (!writeAll(target, separator))
            return {.path = partPath, .fileError = target.error(), .error = target.errorString()};

        bool wroteAny = false;
        bool endedWithLf = false;
        QByteArray buffer;
        buffer.resize(1024 * 1024);
        while (!source.atEnd())
        {
            const auto lineStart = source.pos();
            if (lineNeedsMboxEscape(source, lineStart) && !writeAll(target, QByteArrayView{">", 1}))
            {
                return {
                    .path = partPath, .fileError = target.error(), .error = target.errorString()};
            }
            if (!source.seek(lineStart))
                return {
                    .path = partPath, .fileError = source.error(), .error = source.errorString()};

            bool lineDone = false;
            while (!lineDone)
            {
                const auto count = source.read(buffer.data(), buffer.size());
                if (count < 0)
                    return {.path = partPath,
                            .fileError = source.error(),
                            .error = source.errorString()};
                if (count == 0)
                    break;
                const QByteArrayView view{buffer.constData(), count};
                const auto newline = view.indexOf('\n');
                const auto toWrite = newline >= 0 ? newline + 1 : count;
                if (!writeAll(target, view.first(toWrite)))
                    return {.path = partPath,
                            .fileError = target.error(),
                            .error = target.errorString()};
                wroteAny = true;
                endedWithLf = view[toWrite - 1] == '\n';
                if (newline >= 0)
                {
                    const auto unread = count - toWrite;
                    if (unread > 0 && !source.seek(source.pos() - unread))
                    {
                        return {.path = partPath,
                                .fileError = source.error(),
                                .error = source.errorString()};
                    }
                    lineDone = true;
                }
            }
        }
        if (!wroteAny || !endedWithLf)
        {
            if (!writeAll(target, QByteArrayView{"\n", 1}))
                return {
                    .path = partPath, .fileError = target.error(), .error = target.errorString()};
        }
        if (!writeAll(target, QByteArrayView{"\n", 1}) || !target.flush())
            return {.path = partPath, .fileError = target.error(), .error = target.errorString()};
#ifdef Q_OS_UNIX
        if (target.handle() >= 0 && ::fsync(target.handle()) != 0)
        {
            return {.path = partPath,
                    .fileError = QFileDevice::WriteError,
                    .error = i18n("Could not flush the mbox file to disk.")};
        }
#endif
        return {.path = partPath, .fileError = QFileDevice::NoError, .error = {}};
    }
} // namespace javelin::app
