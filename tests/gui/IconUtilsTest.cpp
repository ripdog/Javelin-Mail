#include "gui/IconUtils.h"

#include <QColor>
#include <QImage>
#include <QPixmap>

#include <algorithm>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("root context-fill Thunderbird icons render with opaque themed colour")
{
    const auto pixmap = javelin::gui::themedSvgPixmap(
        QString::fromUtf8(JAVELIN_TEST_SOURCE_DIR "/res/thunderbird-icons/remote-blocked.svg"),
        QColor{Qt::white}, 16);
    REQUIRE_FALSE(pixmap.isNull());

    const auto image = pixmap.toImage();
    int maximumAlpha = 0;
    for (int y = 0; y < image.height(); ++y)
    {
        for (int x = 0; x < image.width(); ++x)
        {
            maximumAlpha = std::max(maximumAlpha, image.pixelColor(x, y).alpha());
        }
    }

    CHECK(maximumAlpha >= 240);
}
