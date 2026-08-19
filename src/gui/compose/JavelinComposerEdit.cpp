#include "gui/compose/JavelinComposerEdit.h"

#include "gui/compose/ComposeBodyConverter.h"

#include <KPIMTextEdit/MarkupDirector>
#include <KPIMTextEdit/PlainTextMarkupBuilder>
#include <KPIMTextEdit/RichTextComposerControler>
#include <MessageComposer/MessageComposerSettings>
#include <MessageComposer/TextPart>
#include <Sonnet/ConfigDialog>

#include <KLocalizedString>

#include <QIcon>
#include <QMenu>
#include <QMimeData>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>
#include <QUrl>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <utility>

namespace javelin::gui::compose
{

    namespace
    {

        // QTextDocument flattens <blockquote> into symmetric 40 px block margins.
        constexpr qreal emailQuoteMargin = 40.0;

        [[nodiscard]] bool isEmailQuoteBlock(const QTextBlock& block)
        {
            const auto format = block.blockFormat();
            return std::abs(format.leftMargin() - emailQuoteMargin) < 0.01 &&
                   std::abs(format.rightMargin() - emailQuoteMargin) < 0.01;
        }

        [[nodiscard]] bool containsEmailQuote(const QTextDocument& document)
        {
            for (auto block = document.begin(); block.isValid(); block = block.next())
            {
                if (isEmailQuoteBlock(block))
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] QString quotedPlainText(QString plainText, const QTextDocument& document)
        {
            qsizetype searchStart = 0;
            QString firstQuotedText;
            for (auto block = document.begin(); block.isValid(); block = block.next())
            {
                auto blockText = block.text();
                blockText.replace(QChar::LineSeparator, QLatin1Char('\n'));
                if (blockText.trimmed().isEmpty())
                {
                    continue;
                }
                if (isEmailQuoteBlock(block))
                {
                    firstQuotedText = blockText;
                    break;
                }

                const auto blockStart = plainText.indexOf(blockText, searchStart);
                if (blockStart >= 0)
                {
                    searchStart = blockStart + blockText.size();
                }
            }

            if (firstQuotedText.isEmpty())
            {
                return plainText;
            }

            const auto quoteContentStart = plainText.indexOf(firstQuotedText, searchStart);
            if (quoteContentStart < 0)
            {
                return plainText;
            }
            const auto precedingNewline =
                plainText.lastIndexOf(QLatin1Char('\n'), quoteContentStart);
            const auto quoteStart = precedingNewline < 0 ? 0 : precedingNewline + 1;

            auto quotedLines =
                plainText.sliced(quoteStart).split(QLatin1Char('\n'), Qt::KeepEmptyParts);
            for (qsizetype index = 0; index < quotedLines.size(); ++index)
            {
                auto& line = quotedLines[index];
                if (index == quotedLines.size() - 1 && line.isEmpty())
                {
                    continue;
                }
                if (line.startsWith(QLatin1Char('>')))
                {
                    continue;
                }
                line.prepend(line.isEmpty() ? QStringLiteral(">") : QStringLiteral("> "));
            }

            return plainText.first(quoteStart) + quotedLines.join(QLatin1Char('\n'));
        }

        [[nodiscard]] QString quotedHtml(QString html, const QTextDocument& document)
        {
            if (!containsEmailQuote(document))
            {
                return html;
            }

            static const QRegularExpression quotedBlockOpen{
                QStringLiteral("<(?<tag>p|li)\\b[^>]*\\bstyle\\s*=\\s*\"[^\"]*"
                               "\\bmargin-left\\s*:\\s*40(?:\\.0+)?(?:px)?\\s*;[^\"]*"
                               "\\bmargin-right\\s*:\\s*40(?:\\.0+)?(?:px)?\\s*;[^\"]*\"[^>]*>"),
                QRegularExpression::CaseInsensitiveOption};
            const auto firstQuotedBlock = quotedBlockOpen.match(html);
            if (!firstQuotedBlock.hasMatch())
            {
                return html;
            }

            auto quoteStart = firstQuotedBlock.capturedStart();
            if (firstQuotedBlock.captured(QStringLiteral("tag"))
                    .compare(QStringLiteral("li"), Qt::CaseInsensitive) == 0)
            {
                const auto unorderedListOpen =
                    html.lastIndexOf(QStringLiteral("<ul"), quoteStart, Qt::CaseInsensitive);
                const auto orderedListOpen =
                    html.lastIndexOf(QStringLiteral("<ol"), quoteStart, Qt::CaseInsensitive);
                const auto listOpen = std::max(unorderedListOpen, orderedListOpen);
                const auto unorderedListClose =
                    html.lastIndexOf(QStringLiteral("</ul"), quoteStart, Qt::CaseInsensitive);
                const auto orderedListClose =
                    html.lastIndexOf(QStringLiteral("</ol"), quoteStart, Qt::CaseInsensitive);
                if (listOpen > std::max(unorderedListClose, orderedListClose))
                {
                    quoteStart = listOpen;
                }
            }

            auto quoteEnd = html.lastIndexOf(QStringLiteral("</body>"), -1, Qt::CaseInsensitive);
            if (quoteEnd < quoteStart)
            {
                quoteEnd = html.size();
            }

            const auto quote = html.sliced(quoteStart, quoteEnd - quoteStart);
            const auto blockquote =
                QStringLiteral("<blockquote type=\"cite\" class=\"gmail_quote\" "
                               "style=\"margin:0\">%1</blockquote>")
                    .arg(quote);
            html.replace(quoteStart, quoteEnd - quoteStart, blockquote);
            return html;
        }

    } // namespace

    QString sanitizeComposerPasteHtml(QString html)
    {
        static const QRegularExpression styleElement{
            QStringLiteral("<style\\b[^>]*>.*?</style>"),
            QRegularExpression::CaseInsensitiveOption |
                QRegularExpression::DotMatchesEverythingOption};
        static const QRegularExpression styleAttribute{
            QStringLiteral("\\sstyle\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s>]+)"),
            QRegularExpression::CaseInsensitiveOption};
        static const QRegularExpression classAttribute{
            QStringLiteral("\\sclass\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s>]+)"),
            QRegularExpression::CaseInsensitiveOption};
        static const QRegularExpression fontAttribute{
            QStringLiteral("\\s(?:face|color|size)\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s>]+)"),
            QRegularExpression::CaseInsensitiveOption};
        static const QRegularExpression fontOpenTag{QStringLiteral("<font\\b[^>]*>"),
                                                    QRegularExpression::CaseInsensitiveOption};
        static const QRegularExpression fontCloseTag{QStringLiteral("</font\\s*>"),
                                                     QRegularExpression::CaseInsensitiveOption};

        html.remove(styleElement);
        html.remove(styleAttribute);
        html.remove(classAttribute);
        html.remove(fontAttribute);
        html.remove(fontOpenTag);
        html.remove(fontCloseTag);
        return htmlForQtDocument(html);
    }

    JavelinComposerEdit::JavelinComposerEdit(QWidget* parent)
        : MessageComposer::RichTextComposerNg(parent)
    {
        setSpellCheckingSupport(true);
        setActivateLanguageMenu(true);
        setCheckSpellingEnabled(true);
    }

    void JavelinComposerEdit::addExtraMenuEntry(QMenu* menu, const QPoint position)
    {
        Q_UNUSED(position);
        menu->addSeparator();
        auto* configure = menu->addAction(QIcon::fromTheme(QStringLiteral("tools-check-spelling")),
                                          i18n("Configure Spell Checking…"));
        connect(configure, &QAction::triggered, this,
                [this]
                {
                    Sonnet::ConfigDialog dialog(this);
                    if (!spellCheckingLanguage().isEmpty())
                        dialog.setLanguage(spellCheckingLanguage());
                    if (dialog.exec() != QDialog::Accepted)
                        return;
                    if (!dialog.language().isEmpty())
                        setSpellCheckingLanguage(dialog.language());
                    setCheckSpellingEnabled(true);
                });
    }

    void JavelinComposerEdit::fillComposerTextPart(MessageComposer::TextPart* textPart)
    {
        MessageComposer::RichTextComposerNg::fillComposerTextPart(textPart);
        textPart->setCleanPlainText(quotedPlainText(textPart->cleanPlainText(), *document()));

        if (containsEmailQuote(*document()))
        {
            textPart->setCleanHtml(toCleanHtml());
        }
    }

    QString JavelinComposerEdit::toCleanHtml() const
    {
        return quotedHtml(MessageComposer::RichTextComposerNg::toCleanHtml(), *document());
    }

    QString JavelinComposerEdit::toCleanPlainText() const
    {
        QString plainText;
        if (composerControler()->isFormattingUsed() &&
            MessageComposer::MessageComposerSettings::self()->improvePlainTextOfHtmlMessage())
        {
            KPIMTextEdit::PlainTextMarkupBuilder builder;
            KPIMTextEdit::MarkupDirector director{&builder};
            director.processDocument(document());
            plainText = composerControler()->toCleanPlainText(builder.getResult());
        }
        else
        {
            plainText = composerControler()->toCleanPlainText();
        }
        return quotedPlainText(std::move(plainText), *document());
    }

    void JavelinComposerEdit::insertFromMimeData(const QMimeData* source)
    {
        QStringList localFiles;
        if (source->hasUrls())
        {
            for (const auto& url : source->urls())
            {
                if (url.isLocalFile())
                {
                    localFiles.push_back(url.toLocalFile());
                }
            }
        }
        if (!localFiles.empty())
        {
            Q_EMIT attachmentPathsRequested(localFiles);
            return;
        }

        if (source->hasImage() && !source->hasHtml())
        {
            const auto image = qvariant_cast<QImage>(source->imageData());
            if (!image.isNull())
            {
                Q_EMIT inlineImageRequested(image);
                return;
            }
        }

        if (source->hasHtml())
        {
            QMimeData sanitized;
            sanitized.setHtml(sanitizeComposerPasteHtml(source->html()));
            if (source->hasText())
            {
                sanitized.setText(source->text());
            }
            MessageComposer::RichTextComposerNg::insertFromMimeData(&sanitized);
            return;
        }

        MessageComposer::RichTextComposerNg::insertFromMimeData(source);
    }

    bool JavelinComposerEdit::canInsertFromMimeData(const QMimeData* source) const
    {
        if (source->hasImage())
        {
            return true;
        }
        if (source->hasUrls())
        {
            for (const auto& url : source->urls())
            {
                if (url.isLocalFile())
                {
                    return true;
                }
            }
        }
        return MessageComposer::RichTextComposerNg::canInsertFromMimeData(source);
    }

} // namespace javelin::gui::compose
