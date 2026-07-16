#include "gui/messageview/PlainTextLinkifier.h"

#include <QRegularExpression>
#include <QStringView>

namespace javelin::gui::messageview
{
    namespace
    {
        [[nodiscard]] qsizetype digitCount(const QStringView value)
        {
            qsizetype count = 0;
            for (const auto character : value)
            {
                if (character.isDigit())
                {
                    ++count;
                }
            }
            return count;
        }

        [[nodiscard]] QString phoneTarget(const QStringView value)
        {
            QString target;
            target.reserve(value.size());
            for (const auto character : value)
            {
                if (character.isDigit() || (character == QLatin1Char('+') && target.isEmpty()))
                {
                    target.push_back(character);
                }
            }
            return target;
        }
    } // namespace

    QString linkifyPlainText(const QString& text)
    {
        static const QRegularExpression candidateRegex{
            QStringLiteral(
                R"((?<![\w@])(?:https?://|www\.)[^\s<>]+|(?<![\w.+-])[\w.+-]+@[\w-]+(?:\.[\w-]+)+|(?<![\w])\+?(?:\d[\s().-]*){6,}\d(?![\w]))"),
            QRegularExpression::CaseInsensitiveOption |
                QRegularExpression::UseUnicodePropertiesOption};

        QString html = QStringLiteral("<div style=\"white-space: pre-wrap;\">");
        html.reserve(text.size() + 128);

        qsizetype sourcePosition = 0;
        auto matches = candidateRegex.globalMatch(text);
        while (matches.hasNext())
        {
            const auto match = matches.next();
            const auto start = match.capturedStart();
            auto length = match.capturedLength();
            auto candidate = QStringView{text}.mid(start, length);

            const bool isWebLink =
                candidate.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) ||
                candidate.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive) ||
                candidate.startsWith(QStringLiteral("www."), Qt::CaseInsensitive);
            if (isWebLink)
            {
                while (!candidate.isEmpty() && QStringLiteral(".,;:!?").contains(candidate.back()))
                {
                    candidate.chop(1);
                    --length;
                }
                if (!candidate.isEmpty() && candidate.back() == QLatin1Char(')') &&
                    candidate.count(QLatin1Char(')')) > candidate.count(QLatin1Char('(')))
                {
                    candidate.chop(1);
                    --length;
                }
            }

            html += QStringView{text}
                        .mid(sourcePosition, start - sourcePosition)
                        .toString()
                        .toHtmlEscaped();

            QString target;
            if (isWebLink)
            {
                target = candidate.toString();
                if (target.startsWith(QStringLiteral("www."), Qt::CaseInsensitive))
                {
                    target.prepend(QStringLiteral("https://"));
                }
            }
            else if (candidate.contains(QLatin1Char('@')))
            {
                target = QStringLiteral("mailto:") + candidate.toString();
            }
            else if (digitCount(candidate) >= 7)
            {
                target = QStringLiteral("tel:") + phoneTarget(candidate);
            }

            if (target.isEmpty())
            {
                html += candidate.toString().toHtmlEscaped();
            }
            else
            {
                html += QStringLiteral("<a href=\"%1\">%2</a>")
                            .arg(target.toHtmlEscaped(), candidate.toString().toHtmlEscaped());
            }
            sourcePosition = start + length;
        }

        html += QStringView{text}.mid(sourcePosition).toString().toHtmlEscaped();
        html += QStringLiteral("</div>");
        return html;
    }
} // namespace javelin::gui::messageview
