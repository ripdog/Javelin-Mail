#pragma once

#include <QString>

#include <vector>

namespace javelin::gui::shell
{
    [[nodiscard]] const QString& emailContextMenuSeparatorId();
    [[nodiscard]] const std::vector<QString>& supportedEmailContextMenuActionIds();
    [[nodiscard]] const std::vector<QString>& defaultEmailContextMenuLayout();
    [[nodiscard]] std::vector<QString>
    normalizeEmailContextMenuLayout(const std::vector<QString>& layout);
    [[nodiscard]] std::vector<QString>
    effectiveEmailContextMenuLayout(const std::vector<QString>& configuredLayout);
    [[nodiscard]] std::vector<QString>
    emailContextMenuOverrideForLayout(const std::vector<QString>& layout);
} // namespace javelin::gui::shell
