#include "jmap/submission/DraftInlineImageStorage.h"

#include "jmap/cache/MailVault.h"

#include <QBuffer>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("server draft inline images are materialized for the composer",
          "[jmap][submission][compose][inline-image]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());

    QImage image{3, 2, QImage::Format_ARGB32};
    image.fill(Qt::red);
    QByteArray payload;
    QBuffer buffer{&payload};
    REQUIRE(buffer.open(QIODevice::WriteOnly));
    REQUIRE(image.save(&buffer, "PNG"));

    javelin::jmap::submission::DraftAttachment draft{
        .localFilePath = {},
        .displayName = {},
        .mediaType = {},
        .size = 0,
        .blobId = std::string{"blob-1"},
        .inlineDisposition = true,
        .contentId = std::string{"image-1@example.test"},
        .contentHash = std::nullopt,
    };
    const javelin::jmap::AttachmentDownload download{
        .accountId = "account-1",
        .emailId = "draft-1",
        .partId = "2",
        .name = std::string{"inline.png"},
        .mediaType = "image/png",
        .payload = payload,
        .usedCachedInlinePayload = true,
    };

    const auto result = javelin::jmap::submission::materializeDraftInlineImage(
        javelin::jmap::cache::MailVault{directory.path()}, std::move(draft), download);
    REQUIRE(std::holds_alternative<javelin::jmap::submission::DraftAttachment>(result));
    const auto& materialized = std::get<javelin::jmap::submission::DraftAttachment>(result);

    CHECK(materialized.displayName == "inline.png");
    CHECK(materialized.mediaType == "image/png");
    CHECK(materialized.size == static_cast<std::uint64_t>(payload.size()));
    REQUIRE(materialized.contentHash.has_value());
    CHECK_FALSE(materialized.contentHash->empty());
    CHECK(QFileInfo::exists(QString::fromStdString(materialized.localFilePath)));
    CHECK_FALSE(QImage{QString::fromStdString(materialized.localFilePath)}.isNull());
    CHECK(materialized.contentId == std::optional<std::string>{"image-1@example.test"});
    CHECK(materialized.blobId == std::optional<std::string>{"blob-1"});
}
