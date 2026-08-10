#pragma once

#include "jmap/api/MethodEnvelope.h"
#include "jmap/api/RequestBuilder.h"
#include "jmap/domain/MailEntities.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
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

    struct IdentityGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<javelin::jmap::domain::Identity> list;
        std::vector<std::string> notFound;
    };

    struct IdentityChangesResponse
    {
        std::string accountId;
        std::string oldState;
        std::string newState;
        bool hasMoreChanges = false;
        std::vector<std::string> created;
        std::vector<std::string> updated;
        std::vector<std::string> destroyed;
    };

    struct SetError
    {
        std::string type;
        std::optional<std::string> description;
        std::vector<std::string> properties;
    };

    using IdentitySetError = SetError;
    using MailboxSetError = SetError;

    struct MailboxSetCreate
    {
        std::string name;
        std::optional<std::string> parentId;
        std::uint64_t sortOrder = 0;
        bool isSubscribed = true;
    };

    struct MailboxSetUpdate
    {
        std::optional<bool> isSubscribed;
    };

    struct MailboxSetRequest
    {
        std::string accountId;
        std::optional<std::string> ifInState;
        std::unordered_map<std::string, MailboxSetCreate> create;
        std::unordered_map<std::string, MailboxSetUpdate> update;
        std::vector<std::string> destroy;
        bool onDestroyRemoveEmails = false;
    };

    struct MailboxSetResponse
    {
        std::string accountId;
        std::string oldState;
        std::optional<std::string> newState;
        std::unordered_map<std::string, std::string> created;
        std::vector<std::string> updated;
        std::vector<std::string> destroyed;
        std::unordered_map<std::string, MailboxSetError> notCreated;
        std::unordered_map<std::string, MailboxSetError> notUpdated;
        std::unordered_map<std::string, MailboxSetError> notDestroyed;
    };

    struct IdentitySetCreate
    {
        std::string name;
        std::string email;
        std::vector<javelin::jmap::domain::EmailAddress> replyTo;
        std::vector<javelin::jmap::domain::EmailAddress> bcc;
        std::string textSignature;
        std::string htmlSignature;
    };

    struct IdentitySetUpdate
    {
        std::optional<std::string> name;
        std::optional<std::vector<javelin::jmap::domain::EmailAddress>> replyTo;
        std::optional<std::vector<javelin::jmap::domain::EmailAddress>> bcc;
        std::optional<std::string> textSignature;
        std::optional<std::string> htmlSignature;
    };

    struct IdentitySetRequest
    {
        std::string accountId;
        std::optional<std::string> ifInState;
        std::unordered_map<std::string, IdentitySetCreate> create;
        std::unordered_map<std::string, IdentitySetUpdate> update;
        std::vector<std::string> destroy;
    };

    struct IdentitySetResponse
    {
        std::string accountId;
        std::string oldState;
        std::optional<std::string> newState;
        std::unordered_map<std::string, std::string> created;
        std::vector<std::string> updated;
        std::vector<std::string> destroyed;
        std::unordered_map<std::string, IdentitySetError> notCreated;
        std::unordered_map<std::string, IdentitySetError> notUpdated;
        std::unordered_map<std::string, IdentitySetError> notDestroyed;
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
        std::optional<std::string> operatorName = std::nullopt;
        std::vector<EmailQueryFilter> conditions = {};
        std::optional<std::string> inMailbox = std::nullopt;
        std::optional<std::string> text = std::nullopt;
        std::optional<std::string> from = std::nullopt;
        std::optional<std::string> to = std::nullopt;
        std::optional<std::string> cc = std::nullopt;
        std::optional<std::string> bcc = std::nullopt;
        std::optional<std::string> subject = std::nullopt;
        std::optional<std::string> body = std::nullopt;
        std::optional<std::string> hasKeyword = std::nullopt;
        std::optional<std::string> notKeyword = std::nullopt;
        std::optional<bool> hasAttachment = std::nullopt;
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
        std::optional<std::string> anchor;
        std::int64_t anchorOffset = 0;
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
        std::optional<std::uint64_t> limit;
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
        bool calculateTotal = false;
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
        std::optional<std::uint64_t> maxBodyValueBytes;
    };

    struct EmailContentGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<EmailContent> list;
        std::vector<std::string> notFound;
    };

    using EmailPatchValue = std::variant<std::nullptr_t, bool>;

    struct EmailSetUpdate
    {
        std::unordered_map<std::string, EmailPatchValue> patch;
    };

    struct EmailBodyValueCreate
    {
        std::string value;
        bool isTruncated = false;
    };

    struct EmailBodyPartCreate
    {
        std::optional<std::string> partId;
        std::optional<std::string> blobId;
        std::string type;
        std::optional<std::string> name;
        std::optional<std::string> disposition;
        std::optional<std::string> cid;
        std::optional<std::vector<EmailBodyPartCreate>> subParts;
    };

    struct EmailSetCreate
    {
        std::unordered_map<std::string, bool> mailboxIds;
        std::unordered_map<std::string, bool> keywords;
        std::optional<std::vector<javelin::jmap::domain::EmailAddress>> from;
        std::optional<std::vector<javelin::jmap::domain::EmailAddress>> to;
        std::optional<std::vector<javelin::jmap::domain::EmailAddress>> cc;
        std::optional<std::vector<javelin::jmap::domain::EmailAddress>> bcc;
        std::optional<std::vector<javelin::jmap::domain::EmailAddress>> replyTo;
        std::optional<std::string> subject;
        std::optional<std::string> receivedAt;
        std::optional<std::string> sentAt;
        std::optional<std::vector<std::string>> messageId;
        std::optional<std::vector<std::string>> inReplyTo;
        std::optional<std::vector<std::string>> references;
        std::optional<EmailBodyPartCreate> bodyStructure;
        std::unordered_map<std::string, EmailBodyValueCreate> bodyValues;
    };

    struct EmailSetCreated
    {
        std::string id;
        std::string blobId;
        std::string threadId;
        std::uint64_t size = 0;
    };

    struct EmailSetRequest
    {
        std::string accountId;
        std::optional<std::string> ifInState;
        std::unordered_map<std::string, EmailSetCreate> create;
        std::unordered_map<std::string, EmailSetUpdate> update;
        std::vector<std::string> destroy;
    };

    struct EmailSetResponse
    {
        std::string accountId;
        std::string oldState;
        std::string newState;
        std::unordered_map<std::string, EmailSetCreated> created;
        std::vector<std::string> updated;
        std::vector<std::string> destroyed;
        std::vector<std::string> notCreated;
        std::vector<std::string> notUpdated;
        std::vector<std::string> notDestroyed;
    };

    struct EnvelopeAddress
    {
        std::string email;
        std::unordered_map<std::string, std::optional<std::string>> parameters;
    };

    struct EmailSubmissionEnvelope
    {
        std::optional<EnvelopeAddress> mailFrom;
        std::vector<EnvelopeAddress> rcptTo;
    };

    struct EmailSubmissionCreate
    {
        std::string identityId;
        std::string emailId;
        std::optional<EmailSubmissionEnvelope> envelope;
    };

    using EmailSubmissionPatchValue = EmailPatchValue;

    struct EmailSubmissionSetRequest
    {
        std::string accountId;
        std::unordered_map<std::string, EmailSubmissionCreate> create;
        std::unordered_map<std::string, std::unordered_map<std::string, EmailSubmissionPatchValue>>
            onSuccessUpdateEmail;
    };

    struct EmailSubmissionCreated
    {
        std::string id;
    };

    struct EmailSubmissionSetResponse
    {
        std::string accountId;
        std::string oldState;
        std::string newState;
        std::unordered_map<std::string, EmailSubmissionCreated> created;
        std::unordered_map<std::string, SetError> notCreated;
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

    struct MailboxChangesResponse : ChangesResponse
    {
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
    serializeIdentitySetRequest(const IdentitySetRequest& request);
    [[nodiscard]] std::optional<std::string>
    serializeMailboxSetRequest(const MailboxSetRequest& request);
    [[nodiscard]] std::optional<std::string>
    serializeEmailQueryRequest(const EmailQueryRequest& request);
    [[nodiscard]] std::optional<std::string>
    serializeEmailQueryChangesRequest(const EmailQueryChangesRequest& request);
    [[nodiscard]] std::optional<std::string>
    serializeEmailContentGetRequest(const EmailContentGetRequest& request);
    [[nodiscard]] std::optional<std::string>
    serializeEmailSetRequest(const EmailSetRequest& request);
    [[nodiscard]] std::optional<std::string>
    serializeEmailSubmissionSetRequest(const EmailSubmissionSetRequest& request);

    [[nodiscard]] ParsedEnvelope<IdentityGetResponse>
    parseIdentityGetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<IdentityChangesResponse>
    parseIdentityChangesResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<IdentitySetResponse>
    parseIdentitySetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<MailboxGetResponse> parseMailboxGetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<MailboxSetResponse> parseMailboxSetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<EmailGetResponse> parseEmailGetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<ThreadGetResponse> parseThreadGetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<EmailQueryResponse> parseEmailQueryResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<EmailQueryChangesResponse>
    parseEmailQueryChangesResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<EmailContentGetResponse>
    parseEmailContentGetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<EmailSetResponse> parseEmailSetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<EmailSubmissionSetResponse>
    parseEmailSubmissionSetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<ChangesResponse> parseChangesResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<MailboxChangesResponse>
    parseMailboxChangesResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<EmailChangesResponse>
    parseEmailChangesResponse(std::string_view json);

    [[nodiscard]] std::optional<MethodRequest<IdentityGetResponse>>
    identityGet(const GetRequest& request);
    [[nodiscard]] std::optional<MethodRequest<IdentityChangesResponse>>
    identityChanges(const ChangesRequest& request);
    [[nodiscard]] std::optional<MethodRequest<IdentitySetResponse>>
    identitySet(const IdentitySetRequest& request);
    [[nodiscard]] std::optional<MethodRequest<MailboxGetResponse>>
    mailboxGet(const GetRequest& request);
    [[nodiscard]] std::optional<MethodRequest<MailboxSetResponse>>
    mailboxSet(const MailboxSetRequest& request);
    [[nodiscard]] std::optional<MethodRequest<EmailGetResponse>>
    emailGet(const GetRequest& request);
    [[nodiscard]] std::optional<MethodRequest<ThreadGetResponse>>
    threadGet(const GetRequest& request);
    [[nodiscard]] std::optional<MethodRequest<EmailQueryResponse>>
    emailQuery(const EmailQueryRequest& request);
    [[nodiscard]] std::optional<MethodRequest<EmailQueryChangesResponse>>
    emailQueryChanges(const EmailQueryChangesRequest& request);
    [[nodiscard]] std::optional<MethodRequest<MailboxChangesResponse>>
    mailboxChanges(const ChangesRequest& request);
    [[nodiscard]] std::optional<MethodRequest<EmailChangesResponse>>
    emailChanges(const ChangesRequest& request);
    [[nodiscard]] std::optional<MethodRequest<EmailContentGetResponse>>
    emailContentGet(const EmailContentGetRequest& request);
    [[nodiscard]] std::optional<MethodRequest<EmailSetResponse>>
    emailSet(const EmailSetRequest& request);
    [[nodiscard]] std::optional<MethodRequest<EmailSubmissionSetResponse>>
    emailSubmissionSet(const EmailSubmissionSetRequest& request);

    template <typename Response> struct MethodResponseTraits;

    template <> struct MethodResponseTraits<IdentityGetResponse>
    {
        static constexpr std::string_view methodName = "Identity/get";

        [[nodiscard]] static ParsedEnvelope<IdentityGetResponse> parse(std::string_view json)
        {
            return parseIdentityGetResponse(json);
        }
    };

    template <> struct MethodResponseTraits<IdentityChangesResponse>
    {
        static constexpr std::string_view methodName = "Identity/changes";

        [[nodiscard]] static ParsedEnvelope<IdentityChangesResponse> parse(std::string_view json)
        {
            return parseIdentityChangesResponse(json);
        }
    };

    template <> struct MethodResponseTraits<IdentitySetResponse>
    {
        static constexpr std::string_view methodName = "Identity/set";

        [[nodiscard]] static ParsedEnvelope<IdentitySetResponse> parse(std::string_view json)
        {
            return parseIdentitySetResponse(json);
        }
    };

    template <> struct MethodResponseTraits<MailboxGetResponse>
    {
        static constexpr std::string_view methodName = "Mailbox/get";

        [[nodiscard]] static ParsedEnvelope<MailboxGetResponse> parse(std::string_view json)
        {
            return parseMailboxGetResponse(json);
        }
    };

    template <> struct MethodResponseTraits<MailboxSetResponse>
    {
        static constexpr std::string_view methodName = "Mailbox/set";

        [[nodiscard]] static ParsedEnvelope<MailboxSetResponse> parse(std::string_view json)
        {
            return parseMailboxSetResponse(json);
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

    template <> struct MethodResponseTraits<EmailSubmissionSetResponse>
    {
        static constexpr std::string_view methodName = "EmailSubmission/set";

        [[nodiscard]] static ParsedEnvelope<EmailSubmissionSetResponse> parse(std::string_view json)
        {
            return parseEmailSubmissionSetResponse(json);
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

    template <> struct MethodResponseTraits<MailboxChangesResponse>
    {
        static constexpr std::string_view methodName = "Mailbox/changes";

        [[nodiscard]] static ParsedEnvelope<MailboxChangesResponse> parse(std::string_view json)
        {
            return parseMailboxChangesResponse(json);
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
    [[nodiscard]] GetRequest::ResultReference resultReference(const CallHandle<Response>& handle,
                                                              std::string path)
    {
        return GetRequest::ResultReference{
            .resultOf = handle.callId,
            .name = std::string{MethodResponseTraits<Response>::methodName},
            .path = std::move(path),
        };
    }

    template <typename Response>
    [[nodiscard]] GetRequest
    getRequestFrom(std::string accountId, const CallHandle<Response>& handle, std::string path,
                   std::optional<std::vector<std::string>> properties = std::nullopt)
    {
        return GetRequest{
            .accountId = std::move(accountId),
            .ids = std::nullopt,
            .idsReference = resultReference(handle, std::move(path)),
            .properties = std::move(properties),
        };
    }

} // namespace javelin::jmap::api
