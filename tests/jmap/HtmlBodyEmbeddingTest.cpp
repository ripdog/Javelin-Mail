#include "jmap/render/HtmlBodyEmbedding.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("complete HTML documents contribute only their body when embedded",
          "[jmap][submission][html]")
{
    const auto document = QStringLiteral(
        "<!DOCTYPE html><html><head><title>Original</title></head>"
        "<body class=\"mail\"><p>Hello</p><table><tr><td>World</td></tr></table></body></html>");

    const auto embedded = javelin::jmap::render::htmlBodyContentForEmbedding(document);

    CHECK(embedded == QStringLiteral("<p>Hello</p><table><tr><td>World</td></tr></table>"));
    CHECK_FALSE(embedded.contains(QStringLiteral("<html"), Qt::CaseInsensitive));
    CHECK_FALSE(embedded.contains(QStringLiteral("<body"), Qt::CaseInsensitive));
}

TEST_CASE("HTML fragments remain unchanged when embedded", "[jmap][submission][html]")
{
    const auto fragment = QStringLiteral("<p>Hello</p><blockquote>World</blockquote>");
    CHECK(javelin::jmap::render::htmlBodyContentForEmbedding(fragment) == fragment);
}
