#include "jmap/render/HtmlMessageDocumentBuilder.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("html message document builder rewrites cid references to internal urls",
          "[jmap][render][html]")
{
    const std::vector<javelin::jmap::cache::EmailPart> parts{
        {
            .emailId = "eml-1",
            .partId = "3",
            .parentPartId = std::nullopt,
            .blobId = std::optional<std::string>{"blob-inline"},
            .kind = "attachment",
            .mediaType = "image/png",
            .name = std::optional<std::string>{"chart.png"},
            .charset = std::nullopt,
            .disposition = std::optional<std::string>{"inline"},
            .cid = std::optional<std::string>{"chart@cid"},
            .size = 2048,
            .isInlineRenderable = true,
            .isBodySection = false,
        },
    };

    javelin::jmap::render::HtmlMessageDocumentBuilder builder;
    const auto document = builder.build("account-1", "eml-1",
                                        R"(<img src="cid:chart@cid">)", parts);

    CHECK(document.inlineResourceCount == 1);
    CHECK(document.blockedRemoteResourceCount == 0);
    CHECK(document.html.find("javelin-message-inline://message?account=account-1&email=eml-1&part=3&blob=blob-inline") !=
          std::string::npos);
}

TEST_CASE("html message document builder blocks remote resources and strips scripts",
          "[jmap][render][html]")
{
    javelin::jmap::render::HtmlMessageDocumentBuilder builder;
    const auto document =
        builder.build("account-1", "eml-1",
                      R"(<script>alert('x')</script><img src="https://tracker.example.com/pixel.png">)",
                      {});

    CHECK(document.inlineResourceCount == 0);
    CHECK(document.blockedRemoteResourceCount == 1);
    CHECK(document.html.find("<script") == std::string::npos);
    CHECK(document.html.find("data-javelin-blocked-src=\"https://tracker.example.com/pixel.png\"") !=
          std::string::npos);
    CHECK(document.html.find("data-javelin-remote-attr=\"src\"") != std::string::npos);
    CHECK(document.html.find("default-src 'none'") != std::string::npos);
}

TEST_CASE("html message document builder preserves blocked remote styles for live re-enable",
          "[jmap][render][html]")
{
    javelin::jmap::render::HtmlMessageDocumentBuilder builder;
    const auto document = builder.build(
        "account-1", "eml-1",
        R"HTML(<div style="background-image:url(https://images.example.com/banner.png)"></div>)HTML",
        {});

    CHECK(document.blockedRemoteResourceCount == 1);
    CHECK(document.html.find("data-javelin-blocked-style=") != std::string::npos);
    CHECK(document.html.find("data-javelin-disabled-style=") != std::string::npos);
}
