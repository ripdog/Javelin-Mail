#include "gui/compose/JavelinComposerEdit.h"

#include "gui/compose/ComposeBodyConverter.h"

#include <QMimeData>
#include <QRegularExpression>
#include <QUrl>
#include <QVariant>

namespace javelin::gui::compose
{

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
