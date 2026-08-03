#pragma once

#include <string_view>

namespace javelin::jmap::calendar
{
    [[nodiscard]] bool isValidCalendarColor(std::string_view color);
}
