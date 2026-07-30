#include "gui/messageview/MessageAppearance.h"

#include <catch2/catch_test_macros.hpp>

using javelin::gui::messageview::darkReaderThemeColors;
using javelin::gui::messageview::MessageColorMode;
using javelin::gui::messageview::messageColorModeFromStorage;
using javelin::gui::messageview::shouldUseDarkMessageColors;

TEST_CASE("message appearance follows the effective application color scheme", "[gui][message]")
{
    CHECK(shouldUseDarkMessageColors(MessageColorMode::FollowApplication, Qt::ColorScheme::Dark,
                                     false));
    CHECK_FALSE(shouldUseDarkMessageColors(MessageColorMode::FollowApplication,
                                           Qt::ColorScheme::Light, true));
    CHECK(shouldUseDarkMessageColors(MessageColorMode::FollowApplication, Qt::ColorScheme::Unknown,
                                     true));
    CHECK_FALSE(shouldUseDarkMessageColors(MessageColorMode::FollowApplication,
                                           Qt::ColorScheme::Unknown, false));
}

TEST_CASE("message appearance explicit modes override the application", "[gui][message]")
{
    CHECK(shouldUseDarkMessageColors(MessageColorMode::Dark, Qt::ColorScheme::Light, false));
    CHECK_FALSE(shouldUseDarkMessageColors(MessageColorMode::Light, Qt::ColorScheme::Dark, true));
}

TEST_CASE("message appearance rejects unknown persisted modes", "[gui][message]")
{
    CHECK(messageColorModeFromStorage(-1) == MessageColorMode::FollowApplication);
    CHECK(messageColorModeFromStorage(99) == MessageColorMode::FollowApplication);
    CHECK(messageColorModeFromStorage(static_cast<int>(MessageColorMode::Light)) ==
          MessageColorMode::Light);
    CHECK(messageColorModeFromStorage(static_cast<int>(MessageColorMode::Dark)) ==
          MessageColorMode::Dark);
}

TEST_CASE("dark reader colors do not capture the inactive window palette", "[gui][message]")
{
    QPalette palette;
    palette.setColor(QPalette::Active, QPalette::Base, QColor{20, 21, 22});
    palette.setColor(QPalette::Active, QPalette::Text, QColor{235, 236, 237});
    palette.setColor(QPalette::Active, QPalette::Highlight, QColor{40, 120, 210});
    palette.setColor(QPalette::Active, QPalette::Mid, QColor{90, 91, 92});
    palette.setColor(QPalette::Inactive, QPalette::Base, QColor{30, 31, 32});
    palette.setColor(QPalette::Inactive, QPalette::Text, QColor{110, 111, 112});
    palette.setColor(QPalette::Inactive, QPalette::Highlight, QColor{70, 71, 72});
    palette.setColor(QPalette::Inactive, QPalette::Mid, QColor{60, 61, 62});
    palette.setCurrentColorGroup(QPalette::Inactive);

    const auto colors = darkReaderThemeColors(palette);

    CHECK(colors.background == QStringLiteral("#141516"));
    CHECK(colors.text == QStringLiteral("#ebeced"));
    CHECK(colors.selection == QStringLiteral("#2878d2"));
    CHECK(colors.scrollbar == QStringLiteral("#5a5b5c"));
    CHECK(colors.border == QStringLiteral("#5a5b5c"));
}
