#pragma once

#include "jmap/submission/ComposeTypes.h"

#include <QString>

#include <string_view>
#include <vector>

namespace javelin::gui::compose
{

    [[nodiscard]] QString composerContentIdUrl(std::string_view contentId);
    [[nodiscard]] QString composerEditorResourceName(std::string_view contentId);
    [[nodiscard]] QString editorHtmlForInlineAttachments(
        QString html, const std::vector<javelin::jmap::submission::DraftAttachment>& attachments);
    [[nodiscard]] QString stableHtmlForInlineAttachments(
        QString html, const std::vector<javelin::jmap::submission::DraftAttachment>& attachments);
    [[nodiscard]] bool
    reconcileInlineAttachments(std::vector<javelin::jmap::submission::DraftAttachment>& attachments,
                               const QString& stableHtml);

} // namespace javelin::gui::compose
