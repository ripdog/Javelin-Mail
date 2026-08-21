#pragma once

#include "app/FileNameUtils.h"
#include "jmap/domain/MailEntities.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QString>
#include <QStringView>

#include <algorithm>
#include <utility>

namespace javelin::app
{
    [[nodiscard]] inline QString sanitizeMailSaveComponent(QString value,
                                                           const QStringView fallback)
    {
        value = value.trimmed();
        QString sanitized;
        sanitized.reserve(value.size());
        for (const auto character : value)
        {
            const auto unicode = character.unicode();
            const bool invalid = unicode < 0x20 || unicode == 0x7f ||
                                 QStringView{u"<>:\"/\\|?*"}.contains(character);
            if (invalid)
            {
                if (sanitized.isEmpty() || sanitized.back() != QLatin1Char('-'))
                    sanitized.append(QLatin1Char('-'));
                continue;
            }
            sanitized.append(character);
        }
        sanitized = sanitized.simplified();
        while (sanitized.startsWith(QLatin1Char('.')) || sanitized.endsWith(QLatin1Char('.')) ||
               sanitized.endsWith(QLatin1Char(' ')))
        {
            if (sanitized.startsWith(QLatin1Char('.')))
                sanitized.removeFirst();
            else
                sanitized.chop(1);
        }
        if (sanitized.isEmpty())
            sanitized = fallback.toString();
        constexpr qsizetype maximumComponentCharacters = 80;
        while (sanitized.size() > maximumComponentCharacters)
            chopLastUnicodeCodePoint(sanitized);
        sanitized = sanitized.trimmed();
        return sanitized;
    }

    [[nodiscard]] inline QString collisionMailSaveFileName(const QString& fileName,
                                                           const quint64 discriminator,
                                                           const qsizetype maximumUtf8Bytes = 240)
    {
        const QFileInfo info{fileName};
        const QString suffix = info.completeSuffix();
        QString extension = suffix.isEmpty() ? QString{} : QStringLiteral(".") + suffix;
        const QString marker = QStringLiteral("-%1").arg(discriminator);

        while (!extension.isEmpty() &&
               marker.toUtf8().size() + extension.toUtf8().size() > maximumUtf8Bytes)
            chopLastUnicodeCodePoint(extension);

        const auto baseBudget = std::max<qsizetype>(0, maximumUtf8Bytes - marker.toUtf8().size() -
                                                           extension.toUtf8().size());
        QString baseName = info.completeBaseName();
        while (!baseName.isEmpty() && baseName.toUtf8().size() > baseBudget)
            chopLastUnicodeCodePoint(baseName);
        if (baseName.isEmpty() && baseBudget > 0)
        {
            baseName = QStringLiteral("file");
            while (!baseName.isEmpty() && baseName.toUtf8().size() > baseBudget)
                chopLastUnicodeCodePoint(baseName);
        }
        return baseName + marker + extension;
    }

    [[nodiscard]] inline QString
    suggestedMailSaveFileName(const javelin::jmap::domain::Email& email)
    {
        const auto received =
            QDateTime::fromString(QString::fromStdString(email.receivedAt), Qt::ISODate);
        const QString date = received.isValid()
                                 ? received.date().toString(QStringLiteral("yyyy-MM-dd"))
                                 : QStringLiteral("Unknown date");
        QString sender;
        if (!email.from.empty())
        {
            const auto& from = email.from.front();
            sender = from.name.has_value() && !from.name->empty()
                         ? QString::fromStdString(*from.name)
                         : QString::fromStdString(from.email);
        }
        sender = sanitizeMailSaveComponent(std::move(sender), u"Unknown sender");
        const auto subject = sanitizeMailSaveComponent(
            email.subject.has_value() ? QString::fromStdString(*email.subject) : QString{},
            u"No subject");
        const auto stableId =
            QString::fromLatin1(QCryptographicHash::hash(QByteArray::fromStdString(email.id),
                                                         QCryptographicHash::Sha256)
                                    .toHex()
                                    .left(10));
        return truncateGeneratedFileName(
            QStringLiteral("%1 - %2 - %3 - %4.eml").arg(date, sender, subject, stableId));
    }
} // namespace javelin::app
