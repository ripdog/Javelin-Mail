#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

#include <optional>
#include <string>

namespace javelin::gui::mailboxes
{

    [[nodiscard]] QString mailboxIconResource(const std::optional<std::string>& role);
    [[nodiscard]] QIcon mailboxIcon(const std::optional<std::string>& role, const QColor& color);

} // namespace javelin::gui::mailboxes
