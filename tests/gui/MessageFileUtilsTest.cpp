#include "gui/shell/MessageFileUtils.h"

#include <QFile>
#include <QFileInfo>

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
