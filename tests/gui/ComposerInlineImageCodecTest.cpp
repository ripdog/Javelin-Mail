#include "gui/compose/ComposerInlineImageCodec.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace
{
    [[nodiscard]] javelin::jmap::submission::DraftAttachment inlineAttachment()
    {
        return javelin::jmap::submission::DraftAttachment{
            .localFilePath = "/tmp/image.png",
            .displayName = "image.png",
            .mediaType = "image/png",
            .size = 123,
            .blobId = std::nullopt,
            .inlineDisposition = true,
            .contentId = std::string{"javelin-fixed@inline"},
            .contentHash = std::nullopt,
        };
    }

    [[nodiscard]] javelin::jmap::submission::DraftAttachment ordinaryAttachment()
    {
        return javelin::jmap::submission::DraftAttachment{
            .localFilePath = "/tmp/document.pdf",
            .displayName = "document.pdf",
            .mediaType = "application/pdf",
            .size = 456,
            .blobId = std::nullopt,
            .inlineDisposition = false,
            .contentId = std::nullopt,
            .contentHash = std::nullopt,
        };
    }
} // namespace

TEST_CASE("inline image editor resources round-trip to stable Javelin content IDs",
          "[gui][compose][inline-image]")
{
    const std::vector attachments{inlineAttachment()};
    const auto stableHtml =
        QStringLiteral("<p>Before<img src=\"cid:javelin-fixed@inline\">after</p>");

    const auto editorHtml =
        javelin::gui::compose::editorHtmlForInlineAttachments(stableHtml, attachments);
    CHECK(editorHtml.contains(QStringLiteral("javelin-inline:javelin-fixed@inline")));
    CHECK_FALSE(editorHtml.contains(QStringLiteral("cid:javelin-fixed@inline")));

    const auto roundTripped =
        javelin::gui::compose::stableHtmlForInlineAttachments(editorHtml, attachments);
    CHECK(roundTripped == stableHtml);
    CHECK(javelin::gui::compose::stableHtmlForInlineAttachments(roundTripped, attachments) ==
          stableHtml);
    CHECK_FALSE(roundTripped.contains(QStringLiteral("@KDE")));
}

TEST_CASE("referenced inline images retain their stable attachment identity",
          "[gui][compose][inline-image]")
{
    std::vector attachments{inlineAttachment(), ordinaryAttachment()};
    const auto html = QStringLiteral("<img src=\"cid:javelin-fixed@inline\">");

    CHECK_FALSE(javelin::gui::compose::reconcileInlineAttachments(attachments, html));
    REQUIRE(attachments.front().contentId.has_value());
    CHECK(*attachments.front().contentId == "javelin-fixed@inline");
    CHECK(attachments.front().inlineDisposition);
    CHECK_FALSE(attachments.back().inlineDisposition);
    CHECK_FALSE(attachments.back().contentId.has_value());
}

TEST_CASE("deleted inline images become ordinary attachments without affecting other files",
          "[gui][compose][inline-image]")
{
    std::vector attachments{inlineAttachment(), ordinaryAttachment()};

    CHECK(javelin::gui::compose::reconcileInlineAttachments(
        attachments, QStringLiteral("<p>The image was deleted.</p>")));
    CHECK_FALSE(attachments.front().inlineDisposition);
    CHECK_FALSE(attachments.front().contentId.has_value());
    CHECK(attachments.front().displayName == "image.png");
    CHECK(attachments.front().localFilePath == "/tmp/image.png");
    CHECK_FALSE(attachments.back().inlineDisposition);
    CHECK_FALSE(attachments.back().contentId.has_value());
    CHECK(attachments.back().displayName == "document.pdf");
}
