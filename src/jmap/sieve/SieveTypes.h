#pragma once

#include <string>

namespace javelin::jmap::sieve
{
    struct SieveScript
    {
        std::string id;
        std::string name;
        std::string blobId;
        bool isActive = false;
    };
} // namespace javelin::jmap::sieve
