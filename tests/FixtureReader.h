#pragma once

#include <string>
#include <string_view>

namespace javelin::tests
{

    [[nodiscard]] std::string loadFixture(std::string_view relativePath);

} // namespace javelin::tests
