#include "gui/compose/JavelinComposerEdit.h"
#include "gui/compose/ComposerInlineImageCodec.h"

#include <KActionCollection>
#include <KPIMTextEdit/RichTextComposerControler>
#include <KPIMTextEdit/RichTextComposerImages>
#include <MessageComposer/TextPart>

#include <QAction>
#include <QColor>
#include <QImage>
#include <QStringList>
#include <QTextCharFormat>
#include <QTextCursor>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("composer paste sanitization preserves content without foreign styling",
          "[gui][compose][composer]")
{
    const auto sanitized = javelin::gui::compose::sanitizeComposerPasteHtml(QStringLiteral(
        "<style>.foreign { color: red; }</style><p class=\"foreign\" style=\"font-size: 30px\">"
        "<font face=\"Comic Sans MS\" color=\"red\">Hello</font> "
        "<a href=\"https://example.com\">world</a></p>"));

    CHECK_FALSE(sanitized.contains(QStringLiteral("<style"), Qt::CaseInsensitive));
    CHECK_FALSE(sanitized.contains(QStringLiteral("class="), Qt::CaseInsensitive));
    CHECK_FALSE(sanitized.contains(QStringLiteral("style="), Qt::CaseInsensitive));
    CHECK_FALSE(sanitized.contains(QStringLiteral("<font"), Qt::CaseInsensitive));
    CHECK(sanitized.contains(QStringLiteral("Hello")));
    CHECK(sanitized.contains(QStringLiteral("https://example.com")));
}

TEST_CASE("KDE composer detects meaningful formatting", "[gui][compose][composer]")
{
    javelin::gui::compose::JavelinComposerEdit editor;
    editor.activateRichText();
    editor.setTextOrHtml(QStringLiteral("<p>Unformatted text</p>"));
    CHECK_FALSE(editor.composerControler()->isFormattingUsed());

    SECTION("bold text")
    {
        editor.setTextOrHtml(QStringLiteral("<p><strong>Bold</strong></p>"));
        CHECK(editor.composerControler()->isFormattingUsed());
    }
    SECTION("list")
    {
        editor.setTextOrHtml(QStringLiteral("<ul><li>Item</li></ul>"));
        CHECK(editor.composerControler()->isFormattingUsed());
    }
    SECTION("heading")
    {
        editor.setTextOrHtml(QStringLiteral("<h2>Heading</h2>"));
        CHECK(editor.composerControler()->isFormattingUsed());
    }
    SECTION("link")
    {
        editor.setTextOrHtml(QStringLiteral("<p><a href=\"https://example.com\">Link</a></p>"));
        CHECK(editor.composerControler()->isFormattingUsed());
    }
    SECTION("colour")
    {
        QTextCursor cursor{editor.document()};
        cursor.select(QTextCursor::Document);
        QTextCharFormat format;
        format.setForeground(QColor{Qt::red});
        cursor.mergeCharFormat(format);
        CHECK(editor.composerControler()->isFormattingUsed());
    }
    SECTION("table")
    {
        editor.setTextOrHtml(QStringLiteral("<table><tr><td>Cell</td></tr></table>"));
        CHECK(editor.composerControler()->isFormattingUsed());
    }
    SECTION("image")
    {
        editor.setTextOrHtml(QStringLiteral("<p><img src=\"cid:image@example\"></p>"));
        CHECK(editor.composerControler()->isFormattingUsed());
    }
}

TEST_CASE("KDE composer exposes the KMail formatting action set", "[gui][compose][composer]")
{
    javelin::gui::compose::JavelinComposerEdit editor;
    KActionCollection actions{&editor};
    editor.createActions(&actions);

    const QStringList expectedActions{
        QStringLiteral("format_heading_level"),
        QStringLiteral("format_list_style"),
        QStringLiteral("format_font_family"),
        QStringLiteral("format_font_size"),
        QStringLiteral("format_text_bold"),
        QStringLiteral("format_text_italic"),
        QStringLiteral("format_text_underline"),
        QStringLiteral("format_text_strikeout"),
        QStringLiteral("format_text_foreground_color"),
        QStringLiteral("format_text_background_color"),
        QStringLiteral("format_align_left"),
        QStringLiteral("format_align_center"),
        QStringLiteral("format_align_right"),
        QStringLiteral("format_align_justify"),
        QStringLiteral("format_list_indent_more"),
        QStringLiteral("format_list_indent_less"),
        QStringLiteral("manage_link"),
        QStringLiteral("insert_horizontal_rule"),
        QStringLiteral("insert_html"),
        QStringLiteral("insert_table"),
        QStringLiteral("format_list_checkbox"),
        QStringLiteral("format_reset"),
        QStringLiteral("format_painter"),
        QStringLiteral("direction_ltr"),
        QStringLiteral("direction_rtl"),
    };

    for (const auto& actionName : expectedActions)
    {
        INFO(actionName.toStdString());
        CHECK(actions.action(actionName) != nullptr);
    }
}

TEST_CASE("KDE body generation produces matching HTML and plain alternatives",
          "[gui][compose][composer]")
{
    javelin::gui::compose::JavelinComposerEdit editor;
    editor.activateRichText();
    editor.setTextOrHtml(
        QStringLiteral("<p><strong>Hello</strong> <a href=\"https://example.com\">world</a></p>"
                       "<ul><li>One</li><li>Two</li></ul>"));

    MessageComposer::TextPart textPart;
    editor.fillComposerTextPart(&textPart);

    CHECK(textPart.isHtmlUsed());
    INFO(textPart.cleanHtml().toStdString());
    CHECK(textPart.cleanHtml().contains(QStringLiteral("<html"), Qt::CaseInsensitive));
    CHECK(textPart.cleanHtml().contains(QStringLiteral("<body"), Qt::CaseInsensitive));
    CHECK(textPart.cleanHtml().contains(QStringLiteral("Hello")));
    CHECK(textPart.cleanHtml().contains(QStringLiteral("https://example.com")));
    CHECK(textPart.cleanPlainText().contains(QStringLiteral("Hello")));
    CHECK(textPart.cleanPlainText().contains(QStringLiteral("world")));
    CHECK(textPart.cleanPlainText().contains(QStringLiteral("One")));
    CHECK(textPart.cleanPlainText().contains(QStringLiteral("Two")));
}

TEST_CASE("reply quotation survives KDE body generation", "[gui][compose][composer][reply]")
{
    javelin::gui::compose::JavelinComposerEdit editor;
    editor.activateRichText();
    editor.setTextOrHtml(QStringLiteral(
        "<p>Reply text</p><div class=\"moz-cite-prefix\">On Wed, Aug 5, 2026 at 3:18 PM "
        "Mitchell Ferguson &lt;mitchell@example.test&gt; wrote:<br></div>"
        "<blockquote type=\"cite\"><p>Seth here</p><ul><li>Quoted item</li></ul>"
        "<p>After list</p><p>Seth here</p></blockquote>"));

    MessageComposer::TextPart textPart;
    editor.fillComposerTextPart(&textPart);
    const auto html = editor.toCleanHtml();

    INFO(html.toStdString());
    INFO(textPart.cleanPlainText().toStdString());
    CHECK(html.contains(QStringLiteral("<blockquote"), Qt::CaseInsensitive));
    CHECK(html.contains(QStringLiteral("class=\"gmail_quote\""), Qt::CaseInsensitive));
    CHECK(textPart.cleanHtml().contains(QStringLiteral("<blockquote"), Qt::CaseInsensitive));
    CHECK(textPart.cleanPlainText().contains(QStringLiteral("> Seth here")));
    CHECK(textPart.cleanPlainText().contains(QStringLiteral(">      *  Quoted item")));
    CHECK(textPart.cleanPlainText().contains(QStringLiteral("> After list")));

    javelin::gui::compose::JavelinComposerEdit reopenedEditor;
    reopenedEditor.activateRichText();
    reopenedEditor.setTextOrHtml(html);
    MessageComposer::TextPart reopenedTextPart;
    reopenedEditor.fillComposerTextPart(&reopenedTextPart);
    const auto reopenedHtml = reopenedEditor.toCleanHtml();
    CHECK(reopenedHtml.count(QStringLiteral("<blockquote"), Qt::CaseInsensitive) == 1);
    CHECK(reopenedTextPart.cleanPlainText().contains(QStringLiteral("> Seth here")));
}

TEST_CASE("unformatted composer content still has a complete preview document",
          "[gui][compose][composer]")
{
    javelin::gui::compose::JavelinComposerEdit editor;
    editor.activateRichText();
    editor.setPlainText(QStringLiteral("Ordinary unformatted text"));

    MessageComposer::TextPart textPart;
    editor.fillComposerTextPart(&textPart);
    const auto documentHtml = editor.toCleanHtml();

    CHECK_FALSE(documentHtml.isEmpty());
    CHECK(documentHtml.contains(QStringLiteral("<html"), Qt::CaseInsensitive));
    CHECK(documentHtml.contains(QStringLiteral("<body"), Qt::CaseInsensitive));
    CHECK(documentHtml.contains(QStringLiteral("Ordinary unformatted text")));
    CHECK(editor.toPlainText() == QStringLiteral("Ordinary unformatted text"));
    if (!textPart.isHtmlUsed())
    {
        CHECK(textPart.cleanHtml().isEmpty());
    }
}

TEST_CASE("Javelin code formatting survives KDE body generation", "[gui][compose][composer]")
{
    javelin::gui::compose::JavelinComposerEdit editor;
    editor.activateRichText();
    editor.setPlainText(QStringLiteral("std::string"));
    QTextCursor cursor{editor.document()};
    cursor.select(QTextCursor::Document);
    QTextCharFormat format;
    format.setFontFamilies(QStringList{QStringLiteral("monospace")});
    cursor.mergeCharFormat(format);

    MessageComposer::TextPart textPart;
    editor.fillComposerTextPart(&textPart);
    INFO(textPart.cleanHtml().toStdString());
    CHECK(textPart.cleanHtml().contains(QStringLiteral("monospace"), Qt::CaseInsensitive));
}

TEST_CASE("KDE image resources serialize through stable Javelin content IDs",
          "[gui][compose][composer]")
{
    javelin::gui::compose::JavelinComposerEdit editor;
    editor.activateRichText();
    const auto contentId = std::string{"javelin-image@inline"};
    const auto resourceName = javelin::gui::compose::composerEditorResourceName(contentId);
    QImage image{4, 4, QImage::Format_ARGB32};
    image.fill(Qt::red);
    editor.composerControler()->composerImages()->addImageHelper(resourceName, image);

    const std::vector attachments{javelin::jmap::submission::DraftAttachment{
        .localFilePath = "/tmp/image.png",
        .displayName = "image.png",
        .mediaType = "image/png",
        .size = 64,
        .blobId = std::nullopt,
        .inlineDisposition = true,
        .contentId = contentId,
        .contentHash = std::nullopt,
    }};
    const auto stableHtml =
        javelin::gui::compose::stableHtmlForInlineAttachments(editor.toCleanHtml(), attachments);

    CHECK(stableHtml.contains(QStringLiteral("cid:javelin-image@inline")));
    CHECK_FALSE(stableHtml.contains(QStringLiteral("javelin-inline:")));
    CHECK_FALSE(stableHtml.contains(QStringLiteral("@KDE")));
}

TEST_CASE("plain conversion can strip formatting or preserve it as markup",
          "[gui][compose][composer]")
{
    const auto richHtml = QStringLiteral("<p><strong>Bold</strong></p><ul><li>Item</li></ul>");

    javelin::gui::compose::JavelinComposerEdit stripped;
    stripped.activateRichText();
    stripped.setTextOrHtml(richHtml);
    stripped.forcePlainTextMarkup(false);
    stripped.switchToPlainText();

    javelin::gui::compose::JavelinComposerEdit markedUp;
    markedUp.activateRichText();
    markedUp.setTextOrHtml(richHtml);
    markedUp.forcePlainTextMarkup(true);
    markedUp.switchToPlainText();

    CHECK(stripped.toPlainText().contains(QStringLiteral("Bold")));
    CHECK(stripped.toPlainText().contains(QStringLiteral("Item")));
    CHECK(markedUp.toPlainText().contains(QStringLiteral("Bold")));
    CHECK(markedUp.toPlainText().contains(QStringLiteral("Item")));
    CHECK(markedUp.toPlainText() != stripped.toPlainText());
}

TEST_CASE("unchanged plain text restores the original rich document", "[gui][compose][composer]")
{
    javelin::gui::compose::JavelinComposerEdit editor;
    editor.activateRichText();
    editor.setTextOrHtml(QStringLiteral("<p><strong>Bold</strong> text</p>"));
    const auto originalHtml = editor.toCleanHtml();

    editor.forcePlainTextMarkup(false);
    editor.switchToPlainText();
    editor.activateRichText();

    CHECK(editor.toCleanHtml() == originalHtml);
}

TEST_CASE("editing converted plain text invalidates stale rich restoration",
          "[gui][compose][composer]")
{
    javelin::gui::compose::JavelinComposerEdit editor;
    editor.activateRichText();
    editor.setTextOrHtml(QStringLiteral("<p><strong>Bold</strong> text</p>"));
    const auto originalHtml = editor.toCleanHtml();

    editor.forcePlainTextMarkup(false);
    editor.switchToPlainText();
    auto cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral(" changed"));
    editor.activateRichText();

    CHECK(editor.toPlainText().contains(QStringLiteral("changed")));
    CHECK(editor.toCleanHtml() != originalHtml);
}
