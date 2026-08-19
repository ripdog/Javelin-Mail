#pragma once

#include <QFileInfo>
#include <QString>

#include <algorithm>

namespace javelin::app
{
    inline void chopLastUnicodeCodePoint(QString& value)
    {
        if (value.size() >= 2 && value.back().isLowSurrogate() &&
            value.at(value.size() - 2).isHighSurrogate())
        {
            value.chop(2);
            return;
        }
        value.chop(1);
    }

    // Keep generated leaf names below common 255-byte NAME_MAX limits, with enough slack for
    // collision suffixes and filesystem-specific bookkeeping. The limit is UTF-8 bytes, not
    // QString code units, so non-ASCII display names cannot accidentally overflow the filesystem.
    [[nodiscard]] inline QString truncateGeneratedFileName(QString fileName,
                                                           const qsizetype maximumUtf8Bytes = 240)
    {
        if (fileName.toUtf8().size() <= maximumUtf8Bytes)
            return fileName;

        const QFileInfo info{fileName};
        const QString suffix = info.completeSuffix();
        const QString extension = suffix.isEmpty() ? QString{} : QStringLiteral(".") + suffix;
        QString baseName = info.completeBaseName();
        const qsizetype extensionBytes = extension.toUtf8().size();
        const qsizetype baseBudget = std::max<qsizetype>(1, maximumUtf8Bytes - extensionBytes);
        while (!baseName.isEmpty() && baseName.toUtf8().size() > baseBudget)
            chopLastUnicodeCodePoint(baseName);
        if (baseName.isEmpty())
            baseName = QStringLiteral("file");
        return baseName + extension;
    }
} // namespace javelin::app
