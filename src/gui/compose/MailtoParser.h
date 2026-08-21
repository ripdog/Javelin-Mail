#pragma once

#include "gui/compose/EmailAddressText.h"
#include "jmap/domain/MailEntities.h"

#include <QUrl>
#include <QUrlQuery>

#include <optional>
#include <string>
#include <vector>

namespace javelin::gui::compose
{
    struct ParsedMailto
    {
        std::vector<javelin::jmap::domain::EmailAddress> to;
        std::vector<javelin::jmap::domain::EmailAddress> cc;
        std::vector<javelin::jmap::domain::EmailAddress> bcc;
        std::optional<std::string> subject;
        std::optional<std::string> body;
    };

    inline void appendMailtoAddresses(std::vector<javelin::jmap::domain::EmailAddress>& destination,
                                      const QString& value)
    {
        const auto parsed = parseAddressList(value, false);
        if (!parsed.has_value())
            return;
        destination.insert(destination.end(), parsed->begin(), parsed->end());
    }

    [[nodiscard]] inline std::optional<ParsedMailto> parseMailtoUri(const QString& value)
    {
        const QUrl url{value};
        if (!url.isValid() ||
            url.scheme().compare(QStringLiteral("mailto"), Qt::CaseInsensitive) != 0)
        {
            return std::nullopt;
        }

        ParsedMailto parsed;
        appendMailtoAddresses(parsed.to, url.path(QUrl::FullyDecoded));
        const QUrlQuery query{url};
        for (const auto& [name, itemValue] : query.queryItems(QUrl::FullyDecoded))
        {
            if (name.compare(QStringLiteral("to"), Qt::CaseInsensitive) == 0)
                appendMailtoAddresses(parsed.to, itemValue);
            else if (name.compare(QStringLiteral("cc"), Qt::CaseInsensitive) == 0)
                appendMailtoAddresses(parsed.cc, itemValue);
            else if (name.compare(QStringLiteral("bcc"), Qt::CaseInsensitive) == 0)
                appendMailtoAddresses(parsed.bcc, itemValue);
            else if (name.compare(QStringLiteral("subject"), Qt::CaseInsensitive) == 0 &&
                     !parsed.subject.has_value())
                parsed.subject = itemValue.toStdString();
            else if (name.compare(QStringLiteral("body"), Qt::CaseInsensitive) == 0 &&
                     !parsed.body.has_value())
                parsed.body = itemValue.toStdString();
        }
        return parsed;
    }
} // namespace javelin::gui::compose
