#pragma once

#include <QString>

#include <optional>
#include <string>

namespace javelin::app
{

    [[nodiscard]] inline QString subjectForDisplay(const std::optional<std::string>& subject)
    {
        if (!subject.has_value() || subject->empty())
        {
            return QStringLiteral("<No Subject>");
        }

        return QString::fromStdString(*subject);
    }

} // namespace javelin::app
