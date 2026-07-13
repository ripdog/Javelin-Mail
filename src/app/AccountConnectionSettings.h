#pragma once

#include <string>

namespace javelin::app
{

    struct AccountConnectionSettings
    {
        std::string sessionUrl;
        std::string loginEmail;
        std::string apiKey;
        bool forceWebSocket = false;
    };

} // namespace javelin::app
