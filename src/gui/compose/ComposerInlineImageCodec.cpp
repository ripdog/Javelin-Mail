#include "gui/compose/ComposerInlineImageCodec.h"

namespace javelin::gui::compose
{

    QString composerContentIdUrl(const std::string_view contentId)
    {
        return QStringLiteral("cid:%1").arg(QString::fromStdString(std::string{contentId}));
    }

    QString composerEditorResourceName(const std::string_view contentId)
    {
        return QStringLiteral("javelin-inline:%1")
            .arg(QString::fromStdString(std::string{contentId}));
    }

    QString editorHtmlForInlineAttachments(
        QString html, const std::vector<javelin::jmap::submission::DraftAttachment>& attachments)
    {
        for (const auto& attachment : attachments)
        {
            if (!attachment.inlineDisposition || !attachment.contentId.has_value())
            {
                continue;
            }
            html.replace(composerContentIdUrl(*attachment.contentId),
                         composerEditorResourceName(*attachment.contentId));
        }
        return html;
    }

    QString stableHtmlForInlineAttachments(
        QString html, const std::vector<javelin::jmap::submission::DraftAttachment>& attachments)
    {
        for (const auto& attachment : attachments)
        {
            if (!attachment.contentId.has_value())
            {
                continue;
            }
            html.replace(composerEditorResourceName(*attachment.contentId),
                         composerContentIdUrl(*attachment.contentId));
        }
        return html;
    }

    bool
    reconcileInlineAttachments(std::vector<javelin::jmap::submission::DraftAttachment>& attachments,
                               const QString& stableHtml)
    {
        bool changed = false;
        for (auto& attachment : attachments)
        {
            if (!attachment.inlineDisposition || !attachment.contentId.has_value())
            {
                continue;
            }
            if (!stableHtml.contains(composerContentIdUrl(*attachment.contentId)))
            {
                attachment.inlineDisposition = false;
                attachment.contentId.reset();
                changed = true;
            }
        }
        return changed;
    }

} // namespace javelin::gui::compose
