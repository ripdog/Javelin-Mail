#pragma once

#include "storage/DatabaseError.h"

#include <QString>

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{
    struct EmailKeywordMembership
    {
        std::string emailId;
        std::vector<std::string> keywords;

        friend bool operator==(const EmailKeywordMembership&,
                               const EmailKeywordMembership&) = default;
    };

    struct TagDefinition
    {
        std::string accountId;
        std::string keyword;
        QString displayName;
        QString color;
        int sortOrder = 0;

        friend bool operator==(const TagDefinition&, const TagDefinition&) = default;
    };

    class MailTagReader
    {
      public:
        virtual ~MailTagReader() = default;

        [[nodiscard]] virtual std::variant<std::vector<std::string>, DatabaseError>
        listUserKeywords(std::string_view accountId, std::string_view mailboxId = {}) const = 0;
        [[nodiscard]] virtual std::variant<std::vector<std::string>, DatabaseError>
        listTagKeywords(std::string_view accountId) const = 0;
        [[nodiscard]] virtual std::variant<std::vector<EmailKeywordMembership>, DatabaseError>
        listEmailKeywordMemberships(std::string_view accountId,
                                    const std::vector<std::string>& emailIds) const = 0;
        [[nodiscard]] virtual std::variant<std::vector<TagDefinition>, DatabaseError>
        listTagDefinitions(std::string_view accountId) const = 0;
    };

} // namespace javelin::jmap::cache
