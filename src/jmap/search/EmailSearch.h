#pragma once

#include "jmap/api/MailMethods.h"
#include "jmap/query/EmailListSort.h"

#include <optional>
#include <string>
#include <vector>

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
        std::optional<std::string> inMailbox = std::nullopt;
        bool unreadOnly = false;
        bool starredOnly = false;
        bool hasAttachmentOnly = false;
        bool fromContactsOnly = false;
        bool taggedOnly = false;
        std::vector<std::string> tags = {};
        bool matchAllTags = false;
        std::optional<std::string> quickText = std::nullopt;
        bool quickTextSender = true;
        bool quickTextRecipients = true;
        bool quickTextSubject = true;
        bool quickTextBody = false;
    };

    struct EmailSearchResolution
    {
        std::vector<std::string> contactAddresses = {};
        std::vector<std::string> userKeywords = {};
    };

    [[nodiscard]] bool isEmpty(const EmailSearchCriteria& criteria);
    [[nodiscard]] bool isBasicTextSearch(const EmailSearchCriteria& criteria);
    [[nodiscard]] std::string displayString(const EmailSearchCriteria& criteria);
    [[nodiscard]] std::string cacheKey(const EmailSearchCriteria& criteria,
                                       const javelin::jmap::query::EmailListSort& sort);
    [[nodiscard]] javelin::jmap::api::EmailQueryFilter
    toEmailQueryFilter(const EmailSearchCriteria& criteria,
                       const EmailSearchResolution& resolution = {});

} // namespace javelin::jmap::search
