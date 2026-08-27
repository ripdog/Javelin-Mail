#include "gui/shell/MessageFileUtils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("temporary attachment files survive writer scope and preserve their suffix",
          "[gui][files]")
{
    const QByteArray payload{"attachment payload"};
    const auto result =
        javelin::gui::shell::writePayloadToTemporaryFile(QStringLiteral("notes.txt"), payload);

    REQUIRE(result.errorMessage.isEmpty());
    REQUIRE_FALSE(result.path.isEmpty());
    CHECK(QFileInfo{result.path}.fileName().endsWith(QStringLiteral("-notes.txt")));
    CHECK(QFileInfo::exists(result.path));

    QFile file{result.path};
    REQUIRE(file.open(QIODevice::ReadOnly));
    CHECK(file.readAll() == payload);
    file.close();

    CHECK(QFile::remove(result.path));
}

TEST_CASE("external drag files survive the drag and expire from the owned cache",
          "[gui][files][drag]")
{
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    const auto root = temporaryDirectory.filePath(QStringLiteral("drag-out"));
    constexpr qint64 oldCreationTime = 1000;

    const auto oldDirectory =
        javelin::gui::shell::createExternalDragDirectory(root, oldCreationTime);
    REQUIRE(oldDirectory.errorMessage.isEmpty());
    const auto oldFile = javelin::gui::shell::writePayloadToPath(
        QDir{oldDirectory.path}.filePath(QStringLiteral("message.eml")),
        QByteArrayLiteral("raw message"));
    REQUIRE(oldFile.errorMessage.isEmpty());
    const auto urls = javelin::gui::shell::externalDragFileUrls(oldDirectory.path);
    REQUIRE(urls.size() == 1);
    CHECK(urls.front().toLocalFile() == oldFile.path);
    CHECK(QFileInfo::exists(oldFile.path));

    const auto newCreationTime =
        oldCreationTime + javelin::gui::shell::externalDragRetentionMilliseconds + 1;
    const auto newDirectory =
        javelin::gui::shell::createExternalDragDirectory(root, newCreationTime);
    REQUIRE(newDirectory.errorMessage.isEmpty());
    CHECK_FALSE(QFileInfo::exists(oldDirectory.path));
    CHECK(QFileInfo::exists(newDirectory.path));
}
