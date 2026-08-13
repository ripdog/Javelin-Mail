#pragma once

#include <string>

namespace javelin::jmap
{

    struct LiveConnectionSettings
    {
        std::string sessionUrl;
        std::string loginEmail;
        std::string apiKey;
    };

} // namespace javelin::jmap
