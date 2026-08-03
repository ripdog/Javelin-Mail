#pragma once

#include <QString>

class QTextDocument;

namespace javelin::gui::compose
{

    [[nodiscard]] QString cleanHtmlFromDocument(const QTextDocument& document);
    [[nodiscard]] QString htmlFromPlainText(const QString& plainText);
    [[nodiscard]] QString plainTextFromHtml(const QString& html);

} // namespace javelin::gui::compose
