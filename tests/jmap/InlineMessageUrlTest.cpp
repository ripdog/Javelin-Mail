#include "jmap/render/InlineMessageUrl.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("inline message urls round-trip encoded identifiers", "[jmap][render][inline-url]")
{
    const auto url = javelin::jmap::render::buildInlineMessageUrl(
        "account/1", "email 1", "part:3", "blob?4");

    const auto parsed =
        javelin::jmap::render::parseInlineMessageUrl(QUrl{QString::fromStdString(url)});
    REQUIRE(parsed.has_value());
    CHECK(parsed->accountId == "account/1");
    CHECK(parsed->emailId == "email 1");
    CHECK(parsed->partId == "part:3");
    CHECK(parsed->blobId == "blob?4");
}
