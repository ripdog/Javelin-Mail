#include "jmap/search/EmailSearch.h"

#include <algorithm>
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

        void appendKeyBool(std::string& key, const bool value)
        {
            key.push_back(value ? '1' : '0');
            key.push_back(':');
        }

        void appendKeyStrings(std::string& key, const std::vector<std::string>& values)
        {
            key += std::to_string(values.size());
            key.push_back(':');
            for (const auto& value : values)
            {
                key += std::to_string(value.size());
                key.push_back(':');
                key += value;
                key.push_back(':');
            }
        }

        [[nodiscard]] javelin::jmap::api::EmailQueryFilter
        anyOf(std::vector<javelin::jmap::api::EmailQueryFilter> conditions)
        {
            if (conditions.size() == 1)
                return std::move(conditions.front());
            return javelin::jmap::api::EmailQueryFilter{
                .operatorName = "OR",
                .conditions = std::move(conditions),
            };
        }

        [[nodiscard]] javelin::jmap::api::EmailQueryFilter impossibleFilter()
        {
            return javelin::jmap::api::EmailQueryFilter{
                .operatorName = "AND",
                .conditions =
                    {
                        javelin::jmap::api::EmailQueryFilter{.hasKeyword = "$seen"},
                        javelin::jmap::api::EmailQueryFilter{.notKeyword = "$seen"},
                    },
            };
        }

    } // namespace

    bool isEmpty(const EmailSearchCriteria& criteria)
    {
        return !normalized(criteria.text).has_value() && !normalized(criteria.with).has_value() &&
               !normalized(criteria.from).has_value() && !normalized(criteria.to).has_value() &&
               !normalized(criteria.cc).has_value() && !normalized(criteria.bcc).has_value() &&
               !normalized(criteria.subject).has_value() &&
               !normalized(criteria.body).has_value() &&
               !normalized(criteria.inMailbox).has_value() && !criteria.unreadOnly &&
               !criteria.starredOnly && !criteria.hasAttachmentOnly && !criteria.fromContactsOnly &&
               !criteria.taggedOnly && criteria.tags.empty() &&
               !normalized(criteria.quickText).has_value();
    }

    bool isBasicTextSearch(const EmailSearchCriteria& criteria)
    {
        return normalized(criteria.text).has_value() && !normalized(criteria.with).has_value() &&
               !normalized(criteria.from).has_value() && !normalized(criteria.to).has_value() &&
               !normalized(criteria.cc).has_value() && !normalized(criteria.bcc).has_value() &&
               !normalized(criteria.subject).has_value() &&
               !normalized(criteria.body).has_value() &&
               !normalized(criteria.inMailbox).has_value() && !criteria.unreadOnly &&
               !criteria.starredOnly && !criteria.hasAttachmentOnly && !criteria.fromContactsOnly &&
               !criteria.taggedOnly && criteria.tags.empty() &&
               !normalized(criteria.quickText).has_value();
    }

    std::string displayString(const EmailSearchCriteria& criteria)
    {
        const auto text = normalized(criteria.text);
        if (isBasicTextSearch(criteria))
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
        appendDisplayPart(parts, "mailbox", normalized(criteria.inMailbox));
        appendDisplayPart(parts, "quick", normalized(criteria.quickText));
        if (criteria.unreadOnly)
            parts.emplace_back("unread");
        if (criteria.starredOnly)
            parts.emplace_back("starred");
        if (criteria.hasAttachmentOnly)
            parts.emplace_back("attachment");
        if (criteria.fromContactsOnly)
            parts.emplace_back("contact");
        if (criteria.taggedOnly)
            parts.emplace_back("tagged");
        for (const auto& tag : criteria.tags)
            appendDisplayPart(parts, "tag", std::optional<std::string>{tag});

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
        key.reserve(256);
        appendKeyPart(key, criteria.text);
        appendKeyPart(key, criteria.with);
        appendKeyPart(key, criteria.from);
        appendKeyPart(key, criteria.to);
        appendKeyPart(key, criteria.cc);
        appendKeyPart(key, criteria.bcc);
        appendKeyPart(key, criteria.subject);
        appendKeyPart(key, criteria.body);
        appendKeyPart(key, criteria.inMailbox);
        appendKeyBool(key, criteria.unreadOnly);
        appendKeyBool(key, criteria.starredOnly);
        appendKeyBool(key, criteria.hasAttachmentOnly);
        appendKeyBool(key, criteria.fromContactsOnly);
        appendKeyBool(key, criteria.taggedOnly);
        appendKeyStrings(key, criteria.tags);
        appendKeyBool(key, criteria.matchAllTags);
        appendKeyPart(key, criteria.quickText);
        appendKeyBool(key, criteria.quickTextSender);
        appendKeyBool(key, criteria.quickTextRecipients);
        appendKeyBool(key, criteria.quickTextSubject);
        appendKeyBool(key, criteria.quickTextBody);
        key += "|sort:";
        key += std::to_string(static_cast<int>(sort.property));
        key.push_back(':');
        key += std::to_string(static_cast<int>(sort.direction));
        return key;
    }

    javelin::jmap::api::EmailQueryFilter toEmailQueryFilter(const EmailSearchCriteria& criteria,
                                                            const EmailSearchResolution& resolution)
    {
        std::vector<javelin::jmap::api::EmailQueryFilter> conditions;
        if (const auto text = normalized(criteria.text))
        {
            appendCondition(conditions, javelin::jmap::api::EmailQueryFilter{.text = text});
        }
        if (const auto with = normalized(criteria.with))
        {
            appendCondition(conditions, anyOf({
                                            javelin::jmap::api::EmailQueryFilter{.from = with},
                                            javelin::jmap::api::EmailQueryFilter{.to = with},
                                            javelin::jmap::api::EmailQueryFilter{.cc = with},
                                            javelin::jmap::api::EmailQueryFilter{.bcc = with},
                                        }));
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
        if (const auto mailbox = normalized(criteria.inMailbox))
        {
            appendCondition(conditions, javelin::jmap::api::EmailQueryFilter{.inMailbox = mailbox});
        }
        if (criteria.unreadOnly)
        {
            appendCondition(conditions,
                            javelin::jmap::api::EmailQueryFilter{.notKeyword = "$seen"});
        }
        if (criteria.starredOnly)
        {
            appendCondition(conditions,
                            javelin::jmap::api::EmailQueryFilter{.hasKeyword = "$flagged"});
        }
        if (criteria.hasAttachmentOnly)
        {
            appendCondition(conditions,
                            javelin::jmap::api::EmailQueryFilter{.hasAttachment = true});
        }
        if (criteria.fromContactsOnly)
        {
            if (resolution.contactAddresses.empty())
            {
                appendCondition(conditions, impossibleFilter());
            }
            else
            {
                std::vector<javelin::jmap::api::EmailQueryFilter> contactConditions;
                contactConditions.reserve(resolution.contactAddresses.size());
                for (const auto& address : resolution.contactAddresses)
                    contactConditions.push_back(
                        javelin::jmap::api::EmailQueryFilter{.from = address});
                appendCondition(conditions, anyOf(std::move(contactConditions)));
            }
        }

        auto tags = criteria.tags;
        if (criteria.taggedOnly && tags.empty())
            tags = resolution.userKeywords;
        if (criteria.taggedOnly || !tags.empty())
        {
            if (tags.empty())
            {
                appendCondition(conditions, impossibleFilter());
            }
            else if (criteria.matchAllTags)
            {
                for (const auto& tag : tags)
                {
                    appendCondition(conditions,
                                    javelin::jmap::api::EmailQueryFilter{.hasKeyword = tag});
                }
            }
            else
            {
                std::vector<javelin::jmap::api::EmailQueryFilter> tagConditions;
                tagConditions.reserve(tags.size());
                for (const auto& tag : tags)
                    tagConditions.push_back(
                        javelin::jmap::api::EmailQueryFilter{.hasKeyword = tag});
                appendCondition(conditions, anyOf(std::move(tagConditions)));
            }
        }

        if (const auto quickText = normalized(criteria.quickText))
        {
            std::vector<javelin::jmap::api::EmailQueryFilter> textConditions;
            if (criteria.quickTextSender)
                textConditions.push_back(javelin::jmap::api::EmailQueryFilter{.from = quickText});
            if (criteria.quickTextRecipients)
            {
                textConditions.push_back(javelin::jmap::api::EmailQueryFilter{.to = quickText});
                textConditions.push_back(javelin::jmap::api::EmailQueryFilter{.cc = quickText});
                textConditions.push_back(javelin::jmap::api::EmailQueryFilter{.bcc = quickText});
            }
            if (criteria.quickTextSubject)
                textConditions.push_back(
                    javelin::jmap::api::EmailQueryFilter{.subject = quickText});
            if (criteria.quickTextBody)
                textConditions.push_back(javelin::jmap::api::EmailQueryFilter{.body = quickText});
            if (textConditions.empty())
                textConditions.push_back(javelin::jmap::api::EmailQueryFilter{.text = quickText});
            appendCondition(conditions, anyOf(std::move(textConditions)));
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
