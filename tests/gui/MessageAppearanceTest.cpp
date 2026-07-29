#include "gui/messageview/MessageAppearance.h"

#include <catch2/catch_test_macros.hpp>

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
