#include "jmap/search/EmailSearch.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <vector>

namespace javelin::jmap::search
{
    namespace
    {

        [[nodiscard]] std::optional<std::string> normalized(std::optional<std::string> value)
        {
            if (!value.has_value())
            {
                return std::nullopt;
            }

            const auto first = std::ranges::find_if_not(*value, [](const unsigned char character)
                                                        { return std::isspace(character) != 0; });
            const auto last =
                std::find_if_not(value->rbegin(), value->rend(), [](const unsigned char character)
                                 { return std::isspace(character) != 0; })
                    .base();
            if (first >= last)
            {
                return std::nullopt;
            }

            return std::string{first, last};
        }

        [[nodiscard]] std::string quotedSearchTerm(std::string_view value)
        {
            std::string quoted;
            quoted.reserve(value.size() + 2);
            quoted.push_back('"');
            for (const char character : value)
            {
                if (character == '"' || character == '\\')
                {
                    quoted.push_back('\\');
                }
                quoted.push_back(character);
            }
            quoted.push_back('"');
            return quoted;
        }

        void appendDisplayPart(std::vector<std::string>& parts, std::string_view name,
                               const std::optional<std::string>& value)
        {
            if (!value.has_value())
            {
                return;
            }

            std::string part{name};
            part.push_back(':');
            part += quotedSearchTerm(*value);
            parts.push_back(std::move(part));
        }

        void appendCondition(std::vector<javelin::jmap::api::EmailQueryFilter>& conditions,
                             javelin::jmap::api::EmailQueryFilter condition)
        {
            conditions.push_back(std::move(condition));
        }

        void appendKeyPart(std::string& key, const std::optional<std::string>& value)
        {
            const auto normalizedValue = normalized(value);
            if (!normalizedValue.has_value())
            {
                key += "0:";
                return;
            }

            key += std::to_string(normalizedValue->size());
            key.push_back(':');
            key += *normalizedValue;
        }

    } // namespace

    bool isEmpty(const EmailSearchCriteria& criteria)
    {
        return !normalized(criteria.text).has_value() && !normalized(criteria.with).has_value() &&
               !normalized(criteria.from).has_value() && !normalized(criteria.to).has_value() &&
               !normalized(criteria.cc).has_value() && !normalized(criteria.bcc).has_value() &&
               !normalized(criteria.subject).has_value() && !normalized(criteria.body).has_value();
    }

    std::string displayString(const EmailSearchCriteria& criteria)
    {
        const auto text = normalized(criteria.text);
        if (text.has_value() && !normalized(criteria.with).has_value() &&
            !normalized(criteria.from).has_value() && !normalized(criteria.to).has_value() &&
            !normalized(criteria.cc).has_value() && !normalized(criteria.bcc).has_value() &&
            !normalized(criteria.subject).has_value() && !normalized(criteria.body).has_value())
        {
            return *text;
        }

        std::vector<std::string> parts;
        appendDisplayPart(parts, "text", text);
        appendDisplayPart(parts, "with", normalized(criteria.with));
        appendDisplayPart(parts, "from", normalized(criteria.from));
        appendDisplayPart(parts, "to", normalized(criteria.to));
        appendDisplayPart(parts, "cc", normalized(criteria.cc));
        appendDisplayPart(parts, "bcc", normalized(criteria.bcc));
        appendDisplayPart(parts, "subject", normalized(criteria.subject));
        appendDisplayPart(parts, "body", normalized(criteria.body));

        std::string result;
        for (const auto& part : parts)
        {
            if (!result.empty())
            {
                result.push_back(' ');
            }
            result += part;
        }
        return result;
    }

    std::string cacheKey(const EmailSearchCriteria& criteria,
                         const javelin::jmap::query::EmailListSort& sort)
    {
        std::string key;
        key.reserve(128);
        appendKeyPart(key, criteria.text);
        appendKeyPart(key, criteria.with);
        appendKeyPart(key, criteria.from);
        appendKeyPart(key, criteria.to);
        appendKeyPart(key, criteria.cc);
        appendKeyPart(key, criteria.bcc);
        appendKeyPart(key, criteria.subject);
        appendKeyPart(key, criteria.body);
        key += "|sort:";
        key += std::to_string(static_cast<int>(sort.property));
        key.push_back(':');
        key += std::to_string(static_cast<int>(sort.direction));
        return key;
    }

    javelin::jmap::api::EmailQueryFilter toEmailQueryFilter(const EmailSearchCriteria& criteria)
    {
        std::vector<javelin::jmap::api::EmailQueryFilter> conditions;
        if (const auto text = normalized(criteria.text))
        {
            appendCondition(conditions, javelin::jmap::api::EmailQueryFilter{.text = text});
        }
        if (const auto with = normalized(criteria.with))
        {
            appendCondition(conditions,
                            javelin::jmap::api::EmailQueryFilter{
                                .operatorName = "OR",
                                .conditions =
                                    {
                                        javelin::jmap::api::EmailQueryFilter{.from = with},
                                        javelin::jmap::api::EmailQueryFilter{.to = with},
                                        javelin::jmap::api::EmailQueryFilter{.cc = with},
                                        javelin::jmap::api::EmailQueryFilter{.bcc = with},
                                    },
                            });
        }
        if (const auto from = normalized(criteria.from))
        {
            appendCondition(conditions, javelin::jmap::api::EmailQueryFilter{.from = from});
        }
        if (const auto to = normalized(criteria.to))
        {
            appendCondition(conditions, javelin::jmap::api::EmailQueryFilter{.to = to});
        }
        if (const auto cc = normalized(criteria.cc))
        {
            appendCondition(conditions, javelin::jmap::api::EmailQueryFilter{.cc = cc});
        }
        if (const auto bcc = normalized(criteria.bcc))
        {
            appendCondition(conditions, javelin::jmap::api::EmailQueryFilter{.bcc = bcc});
        }
        if (const auto subject = normalized(criteria.subject))
        {
            appendCondition(conditions, javelin::jmap::api::EmailQueryFilter{.subject = subject});
        }
        if (const auto body = normalized(criteria.body))
        {
            appendCondition(conditions, javelin::jmap::api::EmailQueryFilter{.body = body});
        }

        if (conditions.size() == 1)
        {
            return std::move(conditions.front());
        }

        return javelin::jmap::api::EmailQueryFilter{
            .operatorName = "AND",
            .conditions = std::move(conditions),
        };
    }

} // namespace javelin::jmap::search
