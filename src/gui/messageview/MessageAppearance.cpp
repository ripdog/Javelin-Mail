#include "gui/messageview/MessageAppearance.h"

#include <QSettings>

namespace javelin::gui::messageview
{
    namespace
    {
        constexpr auto appearanceGroup = "messageAppearance";
        constexpr auto colorModeKey = "colorMode";
        constexpr auto darkBackground = "#181a1b";
        constexpr auto darkText = "#e8e6e3";
        constexpr auto darkBorder = "#736b5e";

        [[nodiscard]] QString cssColor(const QColor& color)
        {
            return color.name(QColor::HexRgb);
        }
    } // namespace

    MessageAppearanceSettings loadMessageAppearanceSettings()
    {
        QSettings settings;
        settings.beginGroup(QLatin1StringView{appearanceGroup});
        const auto colorMode =
            messageColorModeFromStorage(settings.value(QLatin1StringView{colorModeKey}).toInt());
        settings.endGroup();
        return {.colorMode = colorMode};
    }

    void saveMessageAppearanceSettings(const MessageAppearanceSettings& value)
    {
        QSettings settings;
        settings.beginGroup(QLatin1StringView{appearanceGroup});
        settings.setValue(QLatin1StringView{colorModeKey}, static_cast<int>(value.colorMode));
        settings.endGroup();
        settings.sync();
    }

    DarkReaderThemeColors darkReaderThemeColors(const QPalette& palette)
    {
        const auto background = palette.color(QPalette::Active, QPalette::Base);
        if (background.lightness() >= 128)
        {
            return {
                .background = QString::fromLatin1(darkBackground),
                .text = QString::fromLatin1(darkText),
                .selection = QStringLiteral("auto"),
                .scrollbar = QStringLiteral("auto"),
                .border = QString::fromLatin1(darkBorder),
            };
        }

        return {
            .background = cssColor(background),
            .text = cssColor(palette.color(QPalette::Active, QPalette::Text)),
            .selection = cssColor(palette.color(QPalette::Active, QPalette::Highlight)),
            .scrollbar = cssColor(palette.color(QPalette::Active, QPalette::Mid)),
            .border = cssColor(palette.color(QPalette::Active, QPalette::Mid)),
        };
    }

} // namespace javelin::gui::messageview
