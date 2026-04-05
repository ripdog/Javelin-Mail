#pragma once

#include "jmap/api/MethodEnvelope.h"
#include "jmap/domain/MailEntities.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace javelin::jmap::api
{

    struct GetRequest
    {
        std::string accountId;
        std::optional<std::vector<std::string>> ids;
        std::optional<std::vector<std::string>> properties;
    };

    struct MailboxGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<javelin::jmap::domain::Mailbox> list;
        std::vector<std::string> notFound;
    };

    struct EmailGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<javelin::jmap::domain::Email> list;
        std::vector<std::string> notFound;
    };

    struct ThreadGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<javelin::jmap::domain::Thread> list;
        std::vector<std::string> notFound;
    };

    struct ChangesRequest
    {
        std::string accountId;
        std::string sinceState;
        std::optional<std::uint64_t> maxChanges;
    };

    struct EmailQueryFilter
    {
        std::optional<std::string> inMailbox;
    };

    struct EmailQuerySort
    {
        std::string property;
        bool isAscending = false;
    };

    struct EmailQueryRequest
    {
        std::string accountId;
        std::optional<EmailQueryFilter> filter;
        std::vector<EmailQuerySort> sort;
        std::optional<std::uint64_t> position;
        std::optional<std::uint64_t> limit;
        bool collapseThreads = false;
        bool calculateTotal = false;
    };

    struct EmailQueryResponse
    {
        std::string accountId;
        std::string queryState;
        bool canCalculateChanges = false;
        std::uint64_t position = 0;
        std::vector<std::string> ids;
        std::optional<std::uint64_t> total;
    };

    struct EmailContentBodyPart
    {
        std::optional<std::string> partId;
        std::optional<std::string> blobId;
        std::uint64_t size = 0;
        std::optional<std::string> name;
        std::string type;
        std::optional<std::string> charset;
        std::optional<std::string> disposition;
        std::optional<std::string> cid;
    };

    struct EmailBodyValue
    {
        bool isEncodingProblem = false;
        bool isTruncated = false;
        std::string value;
    };

    struct EmailContent
    {
        std::string id;
        std::vector<EmailContentBodyPart> textBody;
        std::vector<EmailContentBodyPart> htmlBody;
        std::vector<EmailContentBodyPart> attachments;
        std::unordered_map<std::string, EmailBodyValue> bodyValues;
    };

    struct EmailContentGetRequest
    {
        std::string accountId;
        std::vector<std::string> ids;
        std::vector<std::string> properties;
        std::vector<std::string> bodyProperties;
        bool fetchTextBodyValues = false;
        bool fetchHTMLBodyValues = false;
        bool fetchAllBodyValues = false;
        std::uint64_t maxBodyValueBytes = 0;
    };

    struct EmailContentGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<EmailContent> list;
        std::vector<std::string> notFound;
    };

    struct ChangesResponse
    {
        std::string accountId;
        std::string oldState;
        std::string newState;
        bool hasMoreChanges = false;
        std::vector<std::string> created;
        std::vector<std::string> updated;
        std::vector<std::string> destroyed;
    };

    [[nodiscard]] std::optional<std::string> serializeGetRequest(const GetRequest& request);
    [[nodiscard]] std::optional<std::string> serializeChangesRequest(const ChangesRequest& request);
    [[nodiscard]] std::optional<std::string>
    serializeEmailQueryRequest(const EmailQueryRequest& request);
    [[nodiscard]] std::optional<std::string>
    serializeEmailContentGetRequest(const EmailContentGetRequest& request);

    [[nodiscard]] ParsedEnvelope<MailboxGetResponse> parseMailboxGetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<EmailGetResponse> parseEmailGetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<ThreadGetResponse> parseThreadGetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<EmailQueryResponse> parseEmailQueryResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<EmailContentGetResponse>
    parseEmailContentGetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<ChangesResponse> parseChangesResponse(std::string_view json);

} // namespace javelin::jmap::api
