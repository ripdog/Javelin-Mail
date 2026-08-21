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
        if (maximumUtf8Bytes <= 0)
            return {};
        if (fileName.toUtf8().size() <= maximumUtf8Bytes)
            return fileName;

        const auto truncateToBudget = [](QString value, const qsizetype budget)
        {
            while (!value.isEmpty() && value.toUtf8().size() > budget)
                chopLastUnicodeCodePoint(value);
            return value;
        };

        const QFileInfo info{fileName};
        const QString suffix = info.completeSuffix();
        QString extension = suffix.isEmpty() ? QString{} : QStringLiteral(".") + suffix;
        const qsizetype extensionBytes = extension.toUtf8().size();
        if (extensionBytes >= maximumUtf8Bytes)
            extension.clear();

        const qsizetype baseBudget = maximumUtf8Bytes - extension.toUtf8().size();
        QString baseName = truncateToBudget(info.completeBaseName(), baseBudget);
        if (baseName.isEmpty())
            baseName = truncateToBudget(QStringLiteral("file"), baseBudget);
        return baseName + extension;
    }
} // namespace javelin::app
