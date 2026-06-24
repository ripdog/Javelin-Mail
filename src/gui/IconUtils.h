#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>

namespace javelin::gui
{

    [[nodiscard]] QPixmap themedSvgPixmap(const QString& resourcePath, const QColor& color,
                                          int size = 16);
    [[nodiscard]] QIcon themedSvgIcon(const QString& resourcePath, const QColor& color);

} // namespace javelin::gui
