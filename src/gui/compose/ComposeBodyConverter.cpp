#include "gui/compose/ComposeBodyConverter.h"

#include <QRegularExpression>
#include <QStringList>
#include <QTextDocument>

#include <vector>

namespace javelin::gui::compose
{

    namespace
    {

        [[nodiscard]] QString semanticHtmlFromQtSpans(const QString& html)
        {
            static const QRegularExpression spanTag{QStringLiteral("<span\\b([^>]*)>|</span\\s*>"),
                                                    QRegularExpression::CaseInsensitiveOption};
            static const QRegularExpression styleAttribute{
                QStringLiteral("\\bstyle\\s*=\\s*([\"'])(.*?)\\1"),
                QRegularExpression::CaseInsensitiveOption};
            static const QRegularExpression boldStyle{
                QStringLiteral("\\bfont-weight\\s*:\\s*(?:bold|[6-9]00)"),
                QRegularExpression::CaseInsensitiveOption};
            static const QRegularExpression italicStyle{
                QStringLiteral("\\bfont-style\\s*:\\s*italic"),
                QRegularExpression::CaseInsensitiveOption};
            static const QRegularExpression underlineStyle{
                QStringLiteral("\\btext-decoration[^:]*:[^;]*\\bunderline\\b"),
                QRegularExpression::CaseInsensitiveOption};
            static const QRegularExpression strikeStyle{
                QStringLiteral("\\btext-decoration[^:]*:[^;]*\\bline-through\\b"),
                QRegularExpression::CaseInsensitiveOption};
            static const QRegularExpression codeStyle{
                QStringLiteral("\\bfont-family\\s*:\\s*(?:[\\\"']\\s*)?monospace\\b"),
                QRegularExpression::CaseInsensitiveOption};

            QString result;
            result.reserve(html.size());
            std::vector<QString> closingTags;
            qsizetype previousEnd = 0;
            auto matchIterator = spanTag.globalMatch(html);
            while (matchIterator.hasNext())
            {
                const auto match = matchIterator.next();
                result.append(html.sliced(previousEnd, match.capturedStart() - previousEnd));
                previousEnd = match.capturedEnd();

                if (match.capturedView().startsWith(QStringLiteral("</"), Qt::CaseInsensitive))
                {
                    if (!closingTags.empty())
                    {
                        result.append(closingTags.back());
                        closingTags.pop_back();
                    }
                    continue;
                }

                const auto styleMatch = styleAttribute.match(match.captured(1));
                const auto style = styleMatch.hasMatch() ? styleMatch.captured(2) : QString{};
                QString opening;
                QString closing;
                const auto appendTag = [&opening, &closing](const QString& name)
                {
                    opening.append(QStringLiteral("<%1>").arg(name));
                    closing.prepend(QStringLiteral("</%1>").arg(name));
                };
                if (boldStyle.match(style).hasMatch())
                {
                    appendTag(QStringLiteral("strong"));
                }
                if (italicStyle.match(style).hasMatch())
                {
                    appendTag(QStringLiteral("em"));
                }
                if (underlineStyle.match(style).hasMatch())
                {
                    appendTag(QStringLiteral("u"));
                }
                if (strikeStyle.match(style).hasMatch())
                {
                    appendTag(QStringLiteral("s"));
                }
                if (codeStyle.match(style).hasMatch())
                {
                    appendTag(QStringLiteral("code"));
                }
                result.append(opening);
                closingTags.push_back(closing);
            }
            result.append(html.sliced(previousEnd));
            while (!closingTags.empty())
            {
                result.append(closingTags.back());
                closingTags.pop_back();
            }
            return result;
        }

    } // namespace

    QString htmlForQtDocument(QString html)
    {
        static const QRegularExpression codeOpenTag{QStringLiteral("<code\\b[^>]*>"),
                                                    QRegularExpression::CaseInsensitiveOption};
        static const QRegularExpression codeCloseTag{QStringLiteral("</code\\s*>"),
                                                     QRegularExpression::CaseInsensitiveOption};

        html.replace(codeOpenTag, QStringLiteral("<span style=\"font-family: monospace;\">"));
        html.replace(codeCloseTag, QStringLiteral("</span>"));
        return html;
    }

    QString cleanHtmlFromDocument(const QTextDocument& document)
    {
        static const QRegularExpression body{QStringLiteral("<body\\b[^>]*>(.*)</body\\s*>"),
                                             QRegularExpression::CaseInsensitiveOption |
                                                 QRegularExpression::DotMatchesEverythingOption};
        static const QRegularExpression generatedAttribute{
            QStringLiteral("\\s(?:style|class)\\s*=\\s*(\"[^\"]*\"|'[^']*')"),
            QRegularExpression::CaseInsensitiveOption};
        static const QRegularExpression qtAttribute{
            QStringLiteral("\\s-qt-[a-z-]+\\s*=\\s*(\"[^\"]*\"|'[^']*')"),
            QRegularExpression::CaseInsensitiveOption};

        const auto generated = document.toHtml();
        const auto bodyMatch = body.match(generated);
        auto content = bodyMatch.hasMatch() ? bodyMatch.captured(1) : generated;
        content = semanticHtmlFromQtSpans(content);
        content.remove(generatedAttribute);
        content.remove(qtAttribute);
        content.replace(QStringLiteral("<br />"), QStringLiteral("<br>"));
        content.replace(QStringLiteral("<hr />"), QStringLiteral("<hr>"));
        return content.trimmed();
    }

    QString htmlFromPlainText(const QString& plainText)
    {
        QStringList paragraphs;
        const auto lines = plainText.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
        paragraphs.reserve(lines.size());
        for (const auto& line : lines)
        {
            paragraphs.push_back(line.isEmpty()
                                     ? QStringLiteral("<p>&nbsp;</p>")
                                     : QStringLiteral("<p>%1</p>").arg(line.toHtmlEscaped()));
        }
        return paragraphs.join(QLatin1Char('\n'));
    }

    QString plainTextFromHtml(const QString& html)
    {
        QTextDocument document;
        document.setHtml(htmlForQtDocument(html));
        auto lines = document.toPlainText().split(QLatin1Char('\n'), Qt::KeepEmptyParts);
        for (auto& line : lines)
        {
            if (line.trimmed().isEmpty())
            {
                line.clear();
            }
        }
        return lines.join(QLatin1Char('\n'));
    }

} // namespace javelin::gui::compose
