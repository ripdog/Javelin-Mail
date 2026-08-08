#pragma once

#include <QFont>

#include <algorithm>

namespace javelin::gui
{
    [[nodiscard]] inline QFont fontWithSizeDelta(QFont font, const int delta)
    {
        if (delta == 0)
        {
            return font;
        }

        const int pixelSize = font.pixelSize();
        if (pixelSize > 0)
        {
            font.setPixelSize(std::max(1, pixelSize + delta));
            return font;
        }

        const qreal pointSize = font.pointSizeF();
        if (pointSize > 0.0)
        {
            font.setPointSizeF(std::max<qreal>(1.0, pointSize + delta));
        }

        return font;
    }
} // namespace javelin::gui
