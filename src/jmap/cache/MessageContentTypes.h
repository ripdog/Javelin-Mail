#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace javelin::jmap::cache
{

    struct EmailPart
    {
        std::string emailId;
        std::string partId;
        std::optional<std::string> parentPartId;
        std::optional<std::string> blobId;
        std::string kind;
        std::string mediaType;
        std::optional<std::string> name;
        std::optional<std::string> charset;
        std::optional<std::string> disposition;
        std::optional<std::string> cid;
        std::uint64_t size = 0;
        bool isInlineRenderable = false;
        bool isBodySection = false;
    };

} // namespace javelin::jmap::cache
