#include "gui/FontUtils.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("font size adjustment preserves point-sized fonts", "[gui][font]")
{
    QFont font;
    font.setPointSizeF(10.5);

    const auto adjusted = javelin::gui::fontWithSizeDelta(font, 2);

    CHECK(adjusted.pointSizeF() == 12.5);
    CHECK(adjusted.pixelSize() == -1);
}

TEST_CASE("font size adjustment preserves pixel-sized fonts", "[gui][font]")
{
    QFont font;
    font.setPixelSize(13);

    const auto adjusted = javelin::gui::fontWithSizeDelta(font, 2);

    CHECK(adjusted.pixelSize() == 15);
    CHECK(adjusted.pointSizeF() == -1.0);
}

TEST_CASE("font size adjustment never creates an invalid size", "[gui][font]")
{
    QFont pointFont;
    pointFont.setPointSize(2);
    QFont pixelFont;
    pixelFont.setPixelSize(2);

    CHECK(javelin::gui::fontWithSizeDelta(pointFont, -10).pointSizeF() == 1.0);
    CHECK(javelin::gui::fontWithSizeDelta(pixelFont, -10).pixelSize() == 1);
}
