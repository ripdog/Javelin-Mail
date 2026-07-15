#pragma once

#include <cstdint>
#include <string>

namespace javelin::app
{

    struct AccountConnectionSettings
    {
        std::string connectionId;
        std::uint64_t revision = 0;
        std::string sessionUrl;
        std::string loginEmail;
        std::string apiKey;
    };

} // namespace javelin::app
