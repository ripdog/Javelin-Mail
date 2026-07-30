#pragma once

#include <QPalette>
#include <QString>
#include <Qt>

namespace javelin::gui::messageview
{

    enum class MessageColorMode
    {
        FollowApplication,
        Light,
        Dark,
    };

    struct MessageAppearanceSettings
    {
        MessageColorMode colorMode = MessageColorMode::FollowApplication;
    };

    struct DarkReaderThemeColors
    {
        QString background;
        QString text;
        QString selection;
        QString scrollbar;
        QString border;
    };

    [[nodiscard]] MessageAppearanceSettings loadMessageAppearanceSettings();
    void saveMessageAppearanceSettings(const MessageAppearanceSettings& settings);
    [[nodiscard]] DarkReaderThemeColors darkReaderThemeColors(const QPalette& palette);

    [[nodiscard]] constexpr MessageColorMode messageColorModeFromStorage(const int value)
    {
        switch (value)
        {
        case static_cast<int>(MessageColorMode::Light):
            return MessageColorMode::Light;
        case static_cast<int>(MessageColorMode::Dark):
            return MessageColorMode::Dark;
        default:
            return MessageColorMode::FollowApplication;
        }
    }

    [[nodiscard]] constexpr bool shouldUseDarkMessageColors(const MessageColorMode mode,
                                                            const Qt::ColorScheme colorScheme,
                                                            const bool applicationPaletteIsDark)
    {
        switch (mode)
        {
        case MessageColorMode::Light:
            return false;
        case MessageColorMode::Dark:
            return true;
        case MessageColorMode::FollowApplication:
            return colorScheme == Qt::ColorScheme::Dark ||
                   (colorScheme == Qt::ColorScheme::Unknown && applicationPaletteIsDark);
        }
        return false;
    }

} // namespace javelin::gui::messageview
