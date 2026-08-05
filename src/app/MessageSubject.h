#pragma once

#include <KLocalizedString>

#include <QString>

#include <optional>
#include <string>

namespace javelin::app
{

    [[nodiscard]] inline QString subjectForDisplay(const std::optional<std::string>& subject)
    {
        if (!subject.has_value() || subject->empty())
        {
            return i18nc("@item email with no subject", "<No Subject>");
        }

        return QString::fromStdString(*subject);
    }

} // namespace javelin::app
