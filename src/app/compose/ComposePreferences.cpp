#include "app/ComposePreferences.h"

#include <QSettings>

#include <algorithm>

namespace javelin::app
{
    int ComposePreferences::undoSendDelaySeconds()
    {
        QSettings settings;
        return std::clamp(
            settings.value(QStringLiteral("compose/undoSendDelaySeconds"), 10).toInt(), 1, 120);
    }

    void ComposePreferences::setUndoSendDelaySeconds(const int seconds)
    {
        QSettings settings;
        settings.setValue(QStringLiteral("compose/undoSendDelaySeconds"),
                          std::clamp(seconds, 1, 120));
    }
} // namespace javelin::app
