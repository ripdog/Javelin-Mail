#pragma once

#include "jmap/domain/MailEntities.h"

#include <QString>
#include <QStringList>
#include <QStringView>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace javelin::gui::compose
{
    [[nodiscard]] inline QString quotedDisplayName(QString name)
    {
        const bool requiresQuotes =
            name.trimmed() != name ||
            std::ranges::any_of(name,
                                [](const QChar character)
                                {
                                    return character.isSpace()
                                               ? false
                                               : QStringView{u"()<>[]:;@,\\\""}.contains(
                                                     character) ||
                                                     character.unicode() < 0x20;
                                });
        if (!requiresQuotes)
            return name;
        name.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        name.replace(QLatin1Char('"'), QStringLiteral("\\\""));
        return QStringLiteral("\"%1\"").arg(name);
    }

    [[nodiscard]] inline QString displayAddress(const javelin::jmap::domain::EmailAddress& address)
    {
        if (address.name.has_value() && !address.name->empty())
            return QStringLiteral("%1 <%2>").arg(
                quotedDisplayName(QString::fromStdString(*address.name)),
                QString::fromStdString(address.email));
        return QString::fromStdString(address.email);
    }

    [[nodiscard]] inline QString
    formatAddresses(const std::vector<javelin::jmap::domain::EmailAddress>& addresses)
    {
        QStringList result;
        result.reserve(static_cast<qsizetype>(addresses.size()));
        for (const auto& address : addresses)
            result.push_back(displayAddress(address));
        return result.join(QStringLiteral(", "));
    }

    [[nodiscard]] inline QStringList splitAddressTokens(const QString& value)
    {
        QStringList tokens;
        QString current;
        bool quoted = false;
        bool escaped = false;
        int angleDepth = 0;
        for (const auto character : value)
        {
            if (escaped)
            {
                current.append(character);
                escaped = false;
                continue;
            }
            if (quoted && character == QLatin1Char('\\'))
            {
                current.append(character);
                escaped = true;
                continue;
            }
            if (character == QLatin1Char('"'))
            {
                quoted = !quoted;
                current.append(character);
                continue;
            }
            if (!quoted)
            {
                if (character == QLatin1Char('<'))
                    ++angleDepth;
                else if (character == QLatin1Char('>') && angleDepth > 0)
                    --angleDepth;
                else if (angleDepth == 0 &&
                         (character == QLatin1Char(',') || character == QLatin1Char(';')))
                {
                    if (!current.trimmed().isEmpty())
                        tokens.push_back(current.trimmed());
                    current.clear();
                    continue;
                }
            }
            current.append(character);
        }
        if (!current.trimmed().isEmpty())
            tokens.push_back(current.trimmed());
        return tokens;
    }

    [[nodiscard]] inline std::optional<QString> parseDisplayName(QString value)
    {
        value = value.trimmed();
        if (value.isEmpty())
            return QString{};
        if (!value.startsWith(QLatin1Char('"')))
            return value;
        if (value.size() < 2 || !value.endsWith(QLatin1Char('"')))
            return std::nullopt;

        QString decoded;
        bool escaped = false;
        for (qsizetype index = 1; index + 1 < value.size(); ++index)
        {
            const auto character = value.at(index);
            if (escaped)
            {
                decoded.append(character);
                escaped = false;
                continue;
            }
            if (character == QLatin1Char('\\'))
            {
                escaped = true;
                continue;
            }
            if (character == QLatin1Char('"'))
                return std::nullopt;
            decoded.append(character);
        }
        if (escaped)
            return std::nullopt;
        return decoded;
    }

    [[nodiscard]] inline std::optional<javelin::jmap::domain::EmailAddress>
    parseAddressToken(const QString& token)
    {
        const auto trimmed = token.trimmed();
        if (trimmed.isEmpty())
            return std::nullopt;

        bool quoted = false;
        bool escaped = false;
        qsizetype openBracket = -1;
        qsizetype closeBracket = -1;
        for (qsizetype index = 0; index < trimmed.size(); ++index)
        {
            const auto character = trimmed.at(index);
            if (escaped)
            {
                escaped = false;
                continue;
            }
            if (quoted && character == QLatin1Char('\\'))
            {
                escaped = true;
                continue;
            }
            if (character == QLatin1Char('"'))
            {
                quoted = !quoted;
                continue;
            }
            if (quoted)
                continue;
            if (character == QLatin1Char('<'))
            {
                if (openBracket >= 0)
                    return std::nullopt;
                openBracket = index;
            }
            else if (character == QLatin1Char('>'))
            {
                if (openBracket < 0 || closeBracket >= 0)
                    return std::nullopt;
                closeBracket = index;
            }
        }
        if (quoted || escaped)
            return std::nullopt;

        if (openBracket >= 0 || closeBracket >= 0)
        {
            if (openBracket < 0 || closeBracket <= openBracket ||
                !trimmed.mid(closeBracket + 1).trimmed().isEmpty())
                return std::nullopt;
            const auto name = parseDisplayName(trimmed.left(openBracket));
            if (!name.has_value())
                return std::nullopt;
            const auto email =
                trimmed.mid(openBracket + 1, closeBracket - openBracket - 1).trimmed();
            if (!email.contains(QLatin1Char('@')))
                return std::nullopt;
            return javelin::jmap::domain::EmailAddress{
                .name = name->isEmpty() ? std::nullopt
                                        : std::optional<std::string>{name->toStdString()},
                .email = email.toStdString(),
            };
        }

        if (!trimmed.contains(QLatin1Char('@')))
            return std::nullopt;
        return javelin::jmap::domain::EmailAddress{.name = std::nullopt,
                                                   .email = trimmed.toStdString()};
    }

    [[nodiscard]] inline std::optional<std::vector<javelin::jmap::domain::EmailAddress>>
    parseAddressList(const QString& value, const bool rejectInvalid = true)
    {
        std::vector<javelin::jmap::domain::EmailAddress> result;
        for (const auto& token : splitAddressTokens(value))
        {
            const auto parsed = parseAddressToken(token);
            if (!parsed.has_value())
            {
                if (rejectInvalid)
                    return std::nullopt;
                continue;
            }
            result.push_back(*parsed);
        }
        return result;
    }
} // namespace javelin::gui::compose
