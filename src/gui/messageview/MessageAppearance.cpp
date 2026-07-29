#include "gui/messageview/MessageAppearance.h"

#include <QSettings>

namespace javelin::gui::messageview
{
    namespace
    {
        constexpr auto appearanceGroup = "messageAppearance";
        constexpr auto colorModeKey = "colorMode";
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

} // namespace javelin::gui::messageview
