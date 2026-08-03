#include "gui/compose/ComposeBodyConverter.h"

#include <QTextDocument>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("Qt rich text is serialized as a clean HTML fragment", "[gui][compose]")
{
    QTextDocument document;
    document.setHtml(QStringLiteral(
        "<html><body style=\"font-family: System Font\"><p style=\"-qt-block-indent: 0\">"
        "<span style=\"font-weight:700; font-style:italic\">Hello</span></p></body></html>"));

    const auto html = javelin::gui::compose::cleanHtmlFromDocument(document);

    CHECK_FALSE(html.contains(QStringLiteral("<!DOCTYPE"), Qt::CaseInsensitive));
    CHECK_FALSE(html.contains(QStringLiteral("<html"), Qt::CaseInsensitive));
    CHECK_FALSE(html.contains(QStringLiteral("font-family"), Qt::CaseInsensitive));
    CHECK_FALSE(html.contains(QStringLiteral("-qt-"), Qt::CaseInsensitive));
    CHECK(html.contains(QStringLiteral("<strong><em>Hello</em></strong>")));
}

TEST_CASE("plain text lines become escaped HTML paragraphs", "[gui][compose]")
{
    const auto html =
        javelin::gui::compose::htmlFromPlainText(QStringLiteral("First & foremost\n\nLast <line>"));

    CHECK(html ==
          QStringLiteral("<p>First &amp; foremost</p>\n<p>&nbsp;</p>\n<p>Last &lt;line&gt;</p>"));
    CHECK(javelin::gui::compose::plainTextFromHtml(html).toStdString() ==
          std::string{"First & foremost\n\nLast <line>"});
}
