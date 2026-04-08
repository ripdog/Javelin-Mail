#pragma once

#include "jmap/api/MethodEnvelope.h"
#include "jmap/api/RequestBuilder.h"
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
        struct ResultReference
        {
            std::string resultOf;
            std::string name;
            std::string path;
        };

        std::string accountId;
        std::optional<std::vector<std::string>> ids;
        std::optional<ResultReference> idsReference;
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

    struct EmailQueryChangesRequest
    {
        std::string accountId;
        std::string sinceQueryState;
        std::optional<std::uint64_t> maxChanges;
        std::optional<std::string> upToId;
        std::optional<EmailQueryFilter> filter;
        std::vector<EmailQuerySort> sort;
        bool collapseThreads = false;
    };

    struct AddedItem
    {
        std::string id;
        std::uint64_t index = 0;
    };

    struct EmailQueryChangesResponse
    {
        std::string accountId;
        std::string oldQueryState;
        std::string newQueryState;
        std::vector<AddedItem> added;
        std::vector<std::string> removed;
        bool hasMoreChanges = false;
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

    struct EmailSetUpdate
    {
        std::unordered_map<std::string, bool> mailboxIds;
        std::unordered_map<std::string, bool> keywords;
    };

    struct EmailSetRequest
    {
        std::string accountId;
        std::unordered_map<std::string, EmailSetUpdate> update;
    };

    struct EmailSetResponse
    {
        std::string accountId;
        std::string oldState;
        std::string newState;
        std::vector<std::string> updated;
        std::vector<std::string> notUpdated;
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

    struct EmailChangesResponse
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
    serializeEmailQueryChangesRequest(const EmailQueryChangesRequest& request);
    [[nodiscard]] std::optional<std::string>
    serializeEmailContentGetRequest(const EmailContentGetRequest& request);
    [[nodiscard]] std::optional<std::string> serializeEmailSetRequest(const EmailSetRequest& request);

    [[nodiscard]] ParsedEnvelope<MailboxGetResponse> parseMailboxGetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<EmailGetResponse> parseEmailGetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<ThreadGetResponse> parseThreadGetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<EmailQueryResponse> parseEmailQueryResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<EmailQueryChangesResponse>
    parseEmailQueryChangesResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<EmailContentGetResponse>
    parseEmailContentGetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<EmailSetResponse> parseEmailSetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<ChangesResponse> parseChangesResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<EmailChangesResponse> parseEmailChangesResponse(std::string_view json);

    [[nodiscard]] std::optional<MethodRequest<MailboxGetResponse>>
    mailboxGet(const GetRequest& request);
    [[nodiscard]] std::optional<MethodRequest<EmailGetResponse>>
    emailGet(const GetRequest& request);
    [[nodiscard]] std::optional<MethodRequest<ThreadGetResponse>>
    threadGet(const GetRequest& request);
    [[nodiscard]] std::optional<MethodRequest<EmailQueryResponse>>
    emailQuery(const EmailQueryRequest& request);
    [[nodiscard]] std::optional<MethodRequest<EmailQueryChangesResponse>>
    emailQueryChanges(const EmailQueryChangesRequest& request);
    [[nodiscard]] std::optional<MethodRequest<EmailChangesResponse>>
    emailChanges(const ChangesRequest& request);
    [[nodiscard]] std::optional<MethodRequest<EmailContentGetResponse>>
    emailContentGet(const EmailContentGetRequest& request);
    [[nodiscard]] std::optional<MethodRequest<EmailSetResponse>>
    emailSet(const EmailSetRequest& request);

    template <typename Response> struct MethodResponseTraits;

    template <> struct MethodResponseTraits<MailboxGetResponse>
    {
        static constexpr std::string_view methodName = "Mailbox/get";

        [[nodiscard]] static ParsedEnvelope<MailboxGetResponse> parse(std::string_view json)
        {
            return parseMailboxGetResponse(json);
        }
    };

    template <> struct MethodResponseTraits<EmailGetResponse>
    {
        static constexpr std::string_view methodName = "Email/get";

        [[nodiscard]] static ParsedEnvelope<EmailGetResponse> parse(std::string_view json)
        {
            return parseEmailGetResponse(json);
        }
    };

    template <> struct MethodResponseTraits<ThreadGetResponse>
    {
        static constexpr std::string_view methodName = "Thread/get";

        [[nodiscard]] static ParsedEnvelope<ThreadGetResponse> parse(std::string_view json)
        {
            return parseThreadGetResponse(json);
        }
    };

    template <> struct MethodResponseTraits<EmailQueryResponse>
    {
        static constexpr std::string_view methodName = "Email/query";

        [[nodiscard]] static ParsedEnvelope<EmailQueryResponse> parse(std::string_view json)
        {
            return parseEmailQueryResponse(json);
        }
    };

    template <> struct MethodResponseTraits<EmailContentGetResponse>
    {
        static constexpr std::string_view methodName = "Email/get";

        [[nodiscard]] static ParsedEnvelope<EmailContentGetResponse> parse(std::string_view json)
        {
            return parseEmailContentGetResponse(json);
        }
    };

    template <> struct MethodResponseTraits<EmailQueryChangesResponse>
    {
        static constexpr std::string_view methodName = "Email/queryChanges";

        [[nodiscard]] static ParsedEnvelope<EmailQueryChangesResponse> parse(std::string_view json)
        {
            return parseEmailQueryChangesResponse(json);
        }
    };

    template <> struct MethodResponseTraits<EmailSetResponse>
    {
        static constexpr std::string_view methodName = "Email/set";

        [[nodiscard]] static ParsedEnvelope<EmailSetResponse> parse(std::string_view json)
        {
            return parseEmailSetResponse(json);
        }
    };

    template <> struct MethodResponseTraits<EmailChangesResponse>
    {
        static constexpr std::string_view methodName = "Email/changes";

        [[nodiscard]] static ParsedEnvelope<EmailChangesResponse> parse(std::string_view json)
        {
            return parseEmailChangesResponse(json);
        }
    };

    template <> struct MethodResponseTraits<ChangesResponse>
    {
        static constexpr std::string_view methodName = "";

        [[nodiscard]] static ParsedEnvelope<ChangesResponse> parse(std::string_view json)
        {
            return parseChangesResponse(json);
        }
    };

    template <typename Response>
    [[nodiscard]] GetRequest::ResultReference
    resultReference(const CallHandle<Response>& handle, std::string path)
    {
        return GetRequest::ResultReference{
            .resultOf = handle.callId,
            .name = std::string{MethodResponseTraits<Response>::methodName},
            .path = std::move(path),
        };
    }

    template <typename Response>
    [[nodiscard]] GetRequest getRequestFrom(std::string accountId, const CallHandle<Response>& handle,
                                            std::string path,
                                            std::optional<std::vector<std::string>> properties =
                                                std::nullopt)
    {
        return GetRequest{
            .accountId = std::move(accountId),
            .ids = std::nullopt,
            .idsReference = resultReference(handle, std::move(path)),
            .properties = std::move(properties),
        };
    }

} // namespace javelin::jmap::api
