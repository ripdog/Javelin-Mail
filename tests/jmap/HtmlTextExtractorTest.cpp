#include "jmap/render/HtmlTextExtractor.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QString>

#include <memory>

namespace
{
    void ensureCoreApplication()
    {
        if (QCoreApplication::instance() != nullptr)
            return;
        static int argc = 1;
        static char name[] = "html-text-extractor-test";
        static char* argv[]{name, nullptr};
        static const auto application = std::make_unique<QCoreApplication>(argc, argv);
        Q_UNUSED(application);
    }
} // namespace

TEST_CASE("HTML text extraction is safe under QCoreApplication", "[jmap][render][html]")
{
    ensureCoreApplication();

    const auto text = javelin::jmap::render::plainTextFromHtml(QStringLiteral(
        R"(<html><head><style>
             table { border-spacing: 1em; }
             td { padding: 0.5em; font-size: 0 !important; }
           </style></head><body>
           <p style="margin: 1em">Incoming message</p>
           <table><tr><td>First cell</td><td>Second cell</td></tr></table>
           </body></html>)"));

    CHECK(text == QStringLiteral("Incoming message\nFirst cell\tSecond cell"));
}

TEST_CASE("HTML text extraction preserves readable structure and entities", "[jmap][render][html]")
{
    const auto text = javelin::jmap::render::plainTextFromHtml(QStringLiteral(
        R"(<p>Hello&nbsp;<strong>world</strong> &amp; universe; caf&eacute;.</p>
           <ul><li>One</li><li>Two<br>continued</li></ul>)"));

    CHECK(text == QStringLiteral("Hello world & universe; café.\nOne\nTwo\ncontinued"));
}

TEST_CASE("HTML text extraction ignores non-content elements and tolerates malformed markup",
          "[jmap][render][html]")
{
    const auto text = javelin::jmap::render::plainTextFromHtml(QStringLiteral(
        R"(<style>.secret { display: none; }</style>
           <script>alert('not content')</script>
           <p>Before <b>bold</p>After &unknown; &#x1f642;)"));

    CHECK(text == QStringLiteral("Before bold\nAfter &unknown; 🙂"));
}

TEST_CASE("HTML text extraction preserves preformatted whitespace", "[jmap][render][html]")
{
    const auto text = javelin::jmap::render::plainTextFromHtml(
        QStringLiteral("<pre>alpha\n  beta</pre><p>gamma</p>"));

    CHECK(text == QStringLiteral("alpha\n  beta\ngamma"));
}
