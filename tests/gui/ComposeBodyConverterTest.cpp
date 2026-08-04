#include "gui/compose/ComposeBodyConverter.h"

#include <QStringList>
#include <QTextCharFormat>
#include <QTextCursor>
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

TEST_CASE("code formatting survives rich text sanitization", "[gui][compose]")
{
    QTextDocument document;
    document.setHtml(javelin::gui::compose::htmlForQtDocument(
        QStringLiteral("<p>Use <code>std::string</code> here.</p>")));

    const auto html = javelin::gui::compose::cleanHtmlFromDocument(document);

    CHECK(html.contains(QStringLiteral("<code>std::string</code>")));
    CHECK(javelin::gui::compose::plainTextFromHtml(
              QStringLiteral("<p>Use <code>std::string</code> here.</p>")) ==
          QStringLiteral("Use std::string here."));
}

TEST_CASE("Qt monospace formatting is serialized as a code tag", "[gui][compose]")
{
    QTextDocument document;
    document.setPlainText(QStringLiteral("std::string"));
    QTextCursor cursor{&document};
    cursor.select(QTextCursor::Document);
    QTextCharFormat format;
    format.setFontFamilies(QStringList{QStringLiteral("monospace")});
    cursor.mergeCharFormat(format);

    const auto html = javelin::gui::compose::cleanHtmlFromDocument(document);

    CHECK(html.contains(QStringLiteral("<code>std::string</code>")));
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
