#pragma once

#include <QString>

namespace javelin::gui::messageview
{
    [[nodiscard]] QString linkifyPlainText(const QString& text);
}
