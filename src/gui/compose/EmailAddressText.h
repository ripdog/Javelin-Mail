#pragma once

#include "jmap/domain/MailEntities.h"

#include <QString>
#include <QStringList>

#include <optional>
#include <string>
#include <vector>

namespace javelin::gui::compose
{
    [[nodiscard]] inline QString displayAddress(const javelin::jmap::domain::EmailAddress& address)
    {
        if (address.name.has_value() && !address.name->empty())
            return QStringLiteral("%1 <%2>").arg(QString::fromStdString(*address.name),
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

    [[nodiscard]] inline std::optional<javelin::jmap::domain::EmailAddress>
    parseAddressToken(const QString& token)
    {
        const auto trimmed = token.trimmed();
        if (trimmed.isEmpty())
            return std::nullopt;
        const auto openBracket = trimmed.lastIndexOf(QLatin1Char('<'));
        const auto closeBracket = trimmed.lastIndexOf(QLatin1Char('>'));
        if (openBracket >= 0 && closeBracket > openBracket)
        {
            auto name = trimmed.left(openBracket).trimmed();
            name.remove(QLatin1Char('"'));
            const auto email =
                trimmed.mid(openBracket + 1, closeBracket - openBracket - 1).trimmed();
            if (!email.contains(QLatin1Char('@')))
                return std::nullopt;
            return javelin::jmap::domain::EmailAddress{
                .name =
                    name.isEmpty() ? std::nullopt : std::optional<std::string>{name.toStdString()},
                .email = email.toStdString(),
            };
        }
        if (!trimmed.contains(QLatin1Char('@')))
            return std::nullopt;
        return javelin::jmap::domain::EmailAddress{.name = std::nullopt,
                                                   .email = trimmed.toStdString()};
    }
} // namespace javelin::gui::compose
