#pragma once

#include <QUrl>

namespace javelin::gui::messageview
{
    [[nodiscard]] inline bool isSafeExternalMessageUrl(const QUrl& url)
    {
        if (!url.isValid())
            return false;
        const auto scheme = url.scheme();
        return scheme.compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0 ||
               scheme.compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 ||
               scheme.compare(QStringLiteral("mailto"), Qt::CaseInsensitive) == 0;
    }
} // namespace javelin::gui::messageview
