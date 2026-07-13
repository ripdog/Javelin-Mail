#pragma once

#include <string>

namespace javelin::app
{

    struct AccountConnectionSettings
    {
        std::string sessionUrl;
        std::string loginEmail;
        std::string apiKey;
    };

} // namespace javelin::app
