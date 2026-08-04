#include "jmap/render/HtmlBodyEmbedding.h"

#include <QRegularExpression>

namespace javelin::jmap::render
{

    QString htmlBodyContentForEmbedding(const QString& html)
    {
        static const QRegularExpression bodyElement{
            QStringLiteral("<body\\b[^>]*>(.*)</body\\s*>"),
            QRegularExpression::CaseInsensitiveOption |
                QRegularExpression::DotMatchesEverythingOption};

        const auto match = bodyElement.match(html);
        return match.hasMatch() ? match.captured(1) : html;
    }

} // namespace javelin::jmap::render
