#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace javelin::jmap::domain
{
    [[nodiscard]] inline std::string canonicalKeyword(std::string keyword)
    {
        std::ranges::transform(keyword, keyword.begin(), [](const unsigned char value)
                               { return static_cast<char>(std::tolower(value)); });
        return keyword;
    }

    [[nodiscard]] inline bool isValidKeyword(const std::string_view keyword)
    {
        if (keyword.empty() || keyword.size() > 255)
            return false;

        for (const auto value : keyword)
        {
            const auto character = static_cast<unsigned char>(value);
            if (character < 0x21 || character > 0x7e)
                return false;
            switch (character)
            {
            case '(':
            case ')':
            case '{':
            case ']':
            case '%':
            case '*':
            case '"':
            case '\\':
                return false;
            default:
                break;
            }
        }
        return true;
    }

    [[nodiscard]] inline bool hasStandardKeywordSemantics(const std::string_view keyword)
    {
        static constexpr std::array<std::string_view, 9> reserved{
            "$answered", "$draft",   "$flagged",  "$forwarded", "$important",
            "$junk",     "$notjunk", "$phishing", "$seen",
        };
        const auto canonical = canonicalKeyword(std::string{keyword});
        return std::ranges::find(reserved, canonical) != reserved.end();
    }
} // namespace javelin::jmap::domain
