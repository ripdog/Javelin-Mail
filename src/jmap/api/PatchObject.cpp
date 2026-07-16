#include "jmap/api/PatchObject.h"

#include <array>

namespace javelin::jmap::api
{

    namespace
    {

        [[nodiscard]] std::string escapeSegment(const std::string_view segment)
        {
            std::string escaped;
            escaped.reserve(segment.size());
            for (const char character : segment)
            {
                if (character == '~')
                {
                    escaped += "~0";
                }
                else if (character == '/')
                {
                    escaped += "~1";
                }
                else
                {
                    escaped.push_back(character);
                }
            }
            return escaped;
        }

    } // namespace

    std::string patchPath(const std::span<const std::string_view> segments)
    {
        std::string path;
        for (const auto segment : segments)
        {
            if (!path.empty())
            {
                path.push_back('/');
            }
            path += escapeSegment(segment);
        }
        return path;
    }

    std::string patchPath(const std::string_view property, const std::string_view mapKey)
    {
        const std::array segments{property, mapKey};
        return patchPath(segments);
    }

} // namespace javelin::jmap::api
