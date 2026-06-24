#pragma once

#include "jmap/api/MailMethods.h"

#include <optional>
#include <string>

namespace javelin::jmap::search
{

    struct EmailSearchCriteria
    {
        std::optional<std::string> text = std::nullopt;
        std::optional<std::string> with = std::nullopt;
        std::optional<std::string> from = std::nullopt;
        std::optional<std::string> to = std::nullopt;
        std::optional<std::string> cc = std::nullopt;
        std::optional<std::string> bcc = std::nullopt;
        std::optional<std::string> subject = std::nullopt;
        std::optional<std::string> body = std::nullopt;
    };

    [[nodiscard]] bool isEmpty(const EmailSearchCriteria& criteria);
    [[nodiscard]] std::string displayString(const EmailSearchCriteria& criteria);
    [[nodiscard]] javelin::jmap::api::EmailQueryFilter
    toEmailQueryFilter(const EmailSearchCriteria& criteria);

} // namespace javelin::jmap::search
