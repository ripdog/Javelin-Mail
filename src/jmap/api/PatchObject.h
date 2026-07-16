#pragma once

#include <span>
#include <string>
#include <string_view>

namespace javelin::jmap::api
{

    [[nodiscard]] std::string patchPath(std::span<const std::string_view> segments);
    [[nodiscard]] std::string patchPath(std::string_view property, std::string_view mapKey);

} // namespace javelin::jmap::api
