#include "jmap/render/HtmlTextExtractor.h"

#include <QRegularExpression>
#include <QTextDocument>

namespace javelin::jmap::render
{
    QString plainTextFromHtml(QString html)
    {
        // Zero-sized text is common in email preheaders and table-spacing markup. QTextDocument
        // passes those CSS values to QFont::setPixelSize(0), which warns for every declaration.
        // Font size has no bearing on plain-text extraction, so give Qt a valid minimum value.
        static const QRegularExpression zeroFontSize{
            QStringLiteral(
                R"(\bfont-size\s*:\s*[+-]?(?:0+(?:\.0*)?|\.0+)(?:[a-z]+|%)?\s*(?:!important\s*)?(?=;|[}"']|$))"),
            QRegularExpression::CaseInsensitiveOption};
        html.replace(zeroFontSize, QStringLiteral("font-size: 1px"));

        QTextDocument document;
        document.setHtml(html);
        return document.toPlainText();
    }
} // namespace javelin::jmap::render
