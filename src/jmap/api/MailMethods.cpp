#include "jmap/api/MailMethods.h"

#include "jmap/domain/MailEntityParsers.h"

#include <glaze/glaze.hpp>

namespace
{

    struct RawGetRequest
    {
        std::string accountId;
        std::optional<std::vector<std::string>> ids;
        std::optional<javelin::jmap::api::GetRequest::ResultReference> idsReference;
        std::optional<std::vector<std::string>> properties;
    };

    struct RawMailboxGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<glz::generic> list;
        std::vector<std::string> notFound;
    };

    struct RawIdentityGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<glz::generic> list;
        std::vector<std::string> notFound;
    };

    struct RawIdentitySetCreate
    {
        std::string name;
        std::string email;
        std::vector<javelin::jmap::domain::EmailAddress> replyTo;
        std::vector<javelin::jmap::domain::EmailAddress> bcc;
        std::string textSignature;
        std::string htmlSignature;
    };

    struct RawIdentitySetUpdate
    {
        std::optional<std::string> name;
        std::optional<std::vector<javelin::jmap::domain::EmailAddress>> replyTo;
        std::optional<std::vector<javelin::jmap::domain::EmailAddress>> bcc;
        std::optional<std::string> textSignature;
        std::optional<std::string> htmlSignature;
    };

    struct RawIdentitySetRequest
    {
        std::string accountId;
        std::optional<std::string> ifInState;
        std::optional<std::unordered_map<std::string, RawIdentitySetCreate>> create;
        std::optional<std::unordered_map<std::string, RawIdentitySetUpdate>> update;
        std::optional<std::vector<std::string>> destroy;
    };

    struct RawIdentitySetCreated
    {
        std::string id;
    };

    struct RawIdentitySetError
    {
        std::string type;
        std::optional<std::string> description;
        std::vector<std::string> properties;
    };

    struct RawIdentitySetResponse
    {
        std::string accountId;
        std::optional<std::string> oldState;
        std::optional<std::string> newState;
        std::optional<std::unordered_map<std::string, RawIdentitySetCreated>> created;
        std::optional<std::unordered_map<std::string, glz::generic>> updated;
        std::optional<std::vector<std::string>> destroyed;
        std::optional<std::unordered_map<std::string, RawIdentitySetError>> notCreated;
        std::optional<std::unordered_map<std::string, RawIdentitySetError>> notUpdated;
        std::optional<std::unordered_map<std::string, RawIdentitySetError>> notDestroyed;
    };

    struct RawEmailGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<glz::generic> list;
        std::vector<std::string> notFound;
    };

    struct RawThreadGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<glz::generic> list;
        std::vector<std::string> notFound;
    };

    struct RawChangesRequest
    {
        std::string accountId;
        std::string sinceState;
        std::optional<std::uint64_t> maxChanges;
    };

    struct RawEmailQueryFilter
    {
        std::optional<std::string> operatorName = std::nullopt;
        std::optional<std::vector<RawEmailQueryFilter>> conditions = std::nullopt;
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

    struct RawEmailQuerySort
    {
        std::string property;
        bool isAscending = false;
    };

    struct RawEmailQueryRequest
    {
        std::string accountId;
        std::optional<RawEmailQueryFilter> filter;
        std::vector<RawEmailQuerySort> sort;
        std::optional<std::uint64_t> position;
        std::optional<std::string> anchor;
        std::optional<std::int64_t> anchorOffset;
        std::optional<std::uint64_t> limit;
        bool collapseThreads = false;
        bool calculateTotal = false;
    };

    struct RawEmailQueryChangesRequest
    {
        std::string accountId;
        std::string sinceQueryState;
        std::optional<std::uint64_t> maxChanges;
        std::optional<std::string> upToId;
        std::optional<RawEmailQueryFilter> filter;
        std::vector<RawEmailQuerySort> sort;
        bool collapseThreads = false;
        bool calculateTotal = false;
    };

    struct RawEmailContentBodyPart
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

    struct RawEmailBodyValue
    {
        bool isEncodingProblem = false;
        bool isTruncated = false;
        std::string value;
    };

    struct RawEmailContent
    {
        std::string id;
        std::vector<RawEmailContentBodyPart> textBody;
        std::vector<RawEmailContentBodyPart> htmlBody;
        std::vector<RawEmailContentBodyPart> attachments;
        std::unordered_map<std::string, RawEmailBodyValue> bodyValues;
    };

    struct RawEmailContentGetRequest
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

    struct RawEmailContentGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<RawEmailContent> list;
        std::vector<std::string> notFound;
    };

    struct RawEmailBodyValueCreate
    {
        std::string value;
        bool isTruncated = false;
    };

    struct RawEmailBodyPartCreate
    {
        std::optional<std::string> partId;
        std::optional<std::string> blobId;
        std::string type;
        std::optional<std::string> name;
        std::optional<std::string> disposition;
        std::optional<std::string> cid;
        std::optional<std::vector<RawEmailBodyPartCreate>> subParts;
    };

    struct RawEmailSetCreate
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
        std::optional<RawEmailBodyPartCreate> bodyStructure;
        std::unordered_map<std::string, RawEmailBodyValueCreate> bodyValues;
    };

    struct RawEmailSetCreated
    {
        std::string id;
        std::string blobId;
        std::string threadId;
        std::uint64_t size = 0;
    };

    struct RawEmailSetRequest
    {
        std::string accountId;
        std::optional<std::string> ifInState;
        std::optional<std::unordered_map<std::string, RawEmailSetCreate>> create;
        std::unordered_map<std::string,
                           std::unordered_map<std::string, javelin::jmap::api::EmailPatchValue>>
            update;
        std::optional<std::vector<std::string>> destroy;
    };

    struct RawEmailSetResponse
    {
        std::string accountId;
        std::optional<std::string> oldState;
        std::string newState;
        std::optional<std::unordered_map<std::string, RawEmailSetCreated>> created;
        std::optional<std::unordered_map<std::string, glz::generic>> updated;
        std::optional<std::vector<std::string>> destroyed;
        std::optional<std::unordered_map<std::string, glz::generic>> notCreated;
        std::optional<std::unordered_map<std::string, glz::generic>> notUpdated;
        std::optional<std::unordered_map<std::string, glz::generic>> notDestroyed;
    };

    struct RawEnvelopeAddress
    {
        std::string email;
    };

    struct RawEmailSubmissionEnvelope
    {
        std::optional<RawEnvelopeAddress> mailFrom;
        std::vector<RawEnvelopeAddress> rcptTo;
    };

    struct RawEmailSubmissionCreate
    {
        std::string identityId;
        std::string emailId;
        std::optional<RawEmailSubmissionEnvelope> envelope;
    };

    struct RawEmailSubmissionCreated
    {
        std::string id;
    };

    struct RawEmailSubmissionSetRequest
    {
        std::string accountId;
        std::unordered_map<std::string, RawEmailSubmissionCreate> create;
        std::optional<std::unordered_map<
            std::string,
            std::unordered_map<std::string, javelin::jmap::api::EmailSubmissionPatchValue>>>
            onSuccessUpdateEmail;
    };

    struct RawEmailSubmissionSetResponse
    {
        std::string accountId;
        std::optional<std::string> oldState;
        std::string newState;
        std::optional<std::unordered_map<std::string, RawEmailSubmissionCreated>> created;
        std::optional<std::unordered_map<std::string, glz::generic>> notCreated;
    };

    struct RawChangesResponse
    {
        std::string accountId;
        std::string oldState;
        std::string newState;
        bool hasMoreChanges = false;
        std::vector<std::string> created;
        std::vector<std::string> updated;
        std::vector<std::string> destroyed;
    };

    struct RawEmailQueryResponse
    {
        std::string accountId;
        std::string queryState;
        bool canCalculateChanges = false;
        std::uint64_t position = 0;
        std::vector<std::string> ids;
        std::optional<std::uint64_t> total;
        std::optional<std::uint64_t> limit;
    };

    struct RawAddedItem
    {
        std::string id;
        std::uint64_t index = 0;
    };

    struct RawEmailQueryChangesResponse
    {
        std::string accountId;
        std::string oldQueryState;
        std::string newQueryState;
        std::vector<RawAddedItem> added;
        std::vector<std::string> removed;
        bool hasMoreChanges = false;
        std::optional<std::uint64_t> total;
    };

} // namespace

template <> struct glz::meta<RawGetRequest>
{
    using T = RawGetRequest;

    static constexpr auto value = glz::object("accountId", &T::accountId, "ids", &T::ids, "#ids",
                                              &T::idsReference, "properties", &T::properties);
};

template <> struct glz::meta<javelin::jmap::api::GetRequest::ResultReference>
{
    using T = javelin::jmap::api::GetRequest::ResultReference;

    static constexpr auto value =
        glz::object("resultOf", &T::resultOf, "name", &T::name, "path", &T::path);
};

template <> struct glz::meta<RawMailboxGetResponse>
{
    using T = RawMailboxGetResponse;

    static constexpr auto value = glz::object("accountId", &T::accountId, "state", &T::state,
                                              "list", &T::list, "notFound", &T::notFound);
};

template <> struct glz::meta<RawIdentityGetResponse>
{
    using T = RawIdentityGetResponse;

    static constexpr auto value = glz::object("accountId", &T::accountId, "state", &T::state,
                                              "list", &T::list, "notFound", &T::notFound);
};

template <> struct glz::meta<RawIdentitySetCreate>
{
    using T = RawIdentitySetCreate;

    static constexpr auto value =
        glz::object("name", &T::name, "email", &T::email, "replyTo", &T::replyTo, "bcc", &T::bcc,
                    "textSignature", &T::textSignature, "htmlSignature", &T::htmlSignature);
};

template <> struct glz::meta<RawIdentitySetUpdate>
{
    using T = RawIdentitySetUpdate;

    static constexpr auto value =
        glz::object("name", &T::name, "replyTo", &T::replyTo, "bcc", &T::bcc, "textSignature",
                    &T::textSignature, "htmlSignature", &T::htmlSignature);
};

template <> struct glz::meta<RawIdentitySetRequest>
{
    using T = RawIdentitySetRequest;

    static constexpr auto value =
        glz::object("accountId", &T::accountId, "ifInState", &T::ifInState, "create", &T::create,
                    "update", &T::update, "destroy", &T::destroy);
};

template <> struct glz::meta<RawIdentitySetCreated>
{
    using T = RawIdentitySetCreated;

    static constexpr auto value = glz::object("id", &T::id);
};

template <> struct glz::meta<RawIdentitySetError>
{
    using T = RawIdentitySetError;

    static constexpr auto value =
        glz::object("type", &T::type, "description", &T::description, "properties", &T::properties);
};

template <> struct glz::meta<RawIdentitySetResponse>
{
    using T = RawIdentitySetResponse;

    static constexpr auto value = glz::object(
        "accountId", &T::accountId, "oldState", &T::oldState, "newState", &T::newState, "created",
        &T::created, "updated", &T::updated, "destroyed", &T::destroyed, "notCreated",
        &T::notCreated, "notUpdated", &T::notUpdated, "notDestroyed", &T::notDestroyed);
};

template <> struct glz::meta<RawEmailGetResponse>
{
    using T = RawEmailGetResponse;

    static constexpr auto value = glz::object("accountId", &T::accountId, "state", &T::state,
                                              "list", &T::list, "notFound", &T::notFound);
};

template <> struct glz::meta<RawThreadGetResponse>
{
    using T = RawThreadGetResponse;

    static constexpr auto value = glz::object("accountId", &T::accountId, "state", &T::state,
                                              "list", &T::list, "notFound", &T::notFound);
};

template <> struct glz::meta<RawChangesRequest>
{
    using T = RawChangesRequest;

    static constexpr auto value = glz::object("accountId", &T::accountId, "sinceState",
                                              &T::sinceState, "maxChanges", &T::maxChanges);
};

template <> struct glz::meta<RawEmailQueryFilter>
{
    using T = RawEmailQueryFilter;

    static constexpr auto value = glz::object(
        "operator", &T::operatorName, "conditions", &T::conditions, "inMailbox", &T::inMailbox,
        "text", &T::text, "from", &T::from, "to", &T::to, "cc", &T::cc, "bcc", &T::bcc, "subject",
        &T::subject, "body", &T::body, "hasKeyword", &T::hasKeyword, "notKeyword", &T::notKeyword,
        "hasAttachment", &T::hasAttachment);
};

template <> struct glz::meta<RawEmailQuerySort>
{
    using T = RawEmailQuerySort;

    static constexpr auto value =
        glz::object("property", &T::property, "isAscending", &T::isAscending);
};

template <> struct glz::meta<RawEmailQueryRequest>
{
    using T = RawEmailQueryRequest;

    static constexpr auto value = glz::object(
        "accountId", &T::accountId, "filter", &T::filter, "sort", &T::sort, "position",
        &T::position, "anchor", &T::anchor, "anchorOffset", &T::anchorOffset, "limit", &T::limit,
        "collapseThreads", &T::collapseThreads, "calculateTotal", &T::calculateTotal);
};

template <> struct glz::meta<RawEmailQueryChangesRequest>
{
    using T = RawEmailQueryChangesRequest;

    static constexpr auto value = glz::object(
        "accountId", &T::accountId, "sinceQueryState", &T::sinceQueryState, "maxChanges",
        &T::maxChanges, "upToId", &T::upToId, "filter", &T::filter, "sort", &T::sort,
        "collapseThreads", &T::collapseThreads, "calculateTotal", &T::calculateTotal);
};

template <> struct glz::meta<RawEmailContentBodyPart>
{
    using T = RawEmailContentBodyPart;

    static constexpr auto value = glz::object(
        "partId", &T::partId, "blobId", &T::blobId, "size", &T::size, "name", &T::name, "type",
        &T::type, "charset", &T::charset, "disposition", &T::disposition, "cid", &T::cid);
};

template <> struct glz::meta<RawEmailBodyValue>
{
    using T = RawEmailBodyValue;

    static constexpr auto value = glz::object("isEncodingProblem", &T::isEncodingProblem,
                                              "isTruncated", &T::isTruncated, "value", &T::value);
};

template <> struct glz::meta<RawEmailContent>
{
    using T = RawEmailContent;

    static constexpr auto value =
        glz::object("id", &T::id, "textBody", &T::textBody, "htmlBody", &T::htmlBody, "attachments",
                    &T::attachments, "bodyValues", &T::bodyValues);
};

template <> struct glz::meta<RawEmailContentGetRequest>
{
    using T = RawEmailContentGetRequest;

    static constexpr auto value = glz::object(
        "accountId", &T::accountId, "ids", &T::ids, "properties", &T::properties, "bodyProperties",
        &T::bodyProperties, "fetchTextBodyValues", &T::fetchTextBodyValues, "fetchHTMLBodyValues",
        &T::fetchHTMLBodyValues, "fetchAllBodyValues", &T::fetchAllBodyValues, "maxBodyValueBytes",
        &T::maxBodyValueBytes);
};

template <> struct glz::meta<RawEmailContentGetResponse>
{
    using T = RawEmailContentGetResponse;

    static constexpr auto value = glz::object("accountId", &T::accountId, "state", &T::state,
                                              "list", &T::list, "notFound", &T::notFound);
};

template <> struct glz::meta<RawEmailBodyValueCreate>
{
    using T = RawEmailBodyValueCreate;

    static constexpr auto value = glz::object("value", &T::value, "isTruncated", &T::isTruncated);
};

template <> struct glz::meta<RawEmailBodyPartCreate>
{
    using T = RawEmailBodyPartCreate;

    static constexpr auto value =
        glz::object("partId", &T::partId, "blobId", &T::blobId, "type", &T::type, "name", &T::name,
                    "disposition", &T::disposition, "cid", &T::cid, "subParts", &T::subParts);
};

template <> struct glz::meta<RawEmailSetCreate>
{
    using T = RawEmailSetCreate;

    static constexpr auto value =
        glz::object("mailboxIds", &T::mailboxIds, "keywords", &T::keywords, "from", &T::from, "to",
                    &T::to, "cc", &T::cc, "bcc", &T::bcc, "replyTo", &T::replyTo, "subject",
                    &T::subject, "receivedAt", &T::receivedAt, "sentAt", &T::sentAt, "messageId",
                    &T::messageId, "inReplyTo", &T::inReplyTo, "references", &T::references,
                    "bodyStructure", &T::bodyStructure, "bodyValues", &T::bodyValues);
};

template <> struct glz::meta<RawEmailSetCreated>
{
    using T = RawEmailSetCreated;

    static constexpr auto value =
        glz::object("id", &T::id, "blobId", &T::blobId, "threadId", &T::threadId, "size", &T::size);
};

template <> struct glz::meta<RawEmailSetRequest>
{
    using T = RawEmailSetRequest;

    static constexpr auto value =
        glz::object("accountId", &T::accountId, "ifInState", &T::ifInState, "create", &T::create,
                    "update", &T::update, "destroy", &T::destroy);
};

template <> struct glz::meta<RawEmailSetResponse>
{
    using T = RawEmailSetResponse;

    static constexpr auto value = glz::object(
        "accountId", &T::accountId, "oldState", &T::oldState, "newState", &T::newState, "created",
        &T::created, "updated", &T::updated, "destroyed", &T::destroyed, "notCreated",
        &T::notCreated, "notUpdated", &T::notUpdated, "notDestroyed", &T::notDestroyed);
};

template <> struct glz::meta<RawEnvelopeAddress>
{
    using T = RawEnvelopeAddress;

    static constexpr auto value = glz::object("email", &T::email);
};

template <> struct glz::meta<RawEmailSubmissionEnvelope>
{
    using T = RawEmailSubmissionEnvelope;

    static constexpr auto value = glz::object("mailFrom", &T::mailFrom, "rcptTo", &T::rcptTo);
};

template <> struct glz::meta<RawEmailSubmissionCreate>
{
    using T = RawEmailSubmissionCreate;

    static constexpr auto value =
        glz::object("identityId", &T::identityId, "emailId", &T::emailId, "envelope", &T::envelope);
};

template <> struct glz::meta<RawEmailSubmissionCreated>
{
    using T = RawEmailSubmissionCreated;

    static constexpr auto value = glz::object("id", &T::id);
};

template <> struct glz::meta<RawEmailSubmissionSetRequest>
{
    using T = RawEmailSubmissionSetRequest;

    static constexpr auto value = glz::object("accountId", &T::accountId, "create", &T::create,
                                              "onSuccessUpdateEmail", &T::onSuccessUpdateEmail);
};

template <> struct glz::meta<RawEmailSubmissionSetResponse>
{
    using T = RawEmailSubmissionSetResponse;

    static constexpr auto value =
        glz::object("accountId", &T::accountId, "oldState", &T::oldState, "newState", &T::newState,
                    "created", &T::created, "notCreated", &T::notCreated);
};

template <> struct glz::meta<RawChangesResponse>
{
    using T = RawChangesResponse;

    static constexpr auto value =
        glz::object("accountId", &T::accountId, "oldState", &T::oldState, "newState", &T::newState,
                    "hasMoreChanges", &T::hasMoreChanges, "created", &T::created, "updated",
                    &T::updated, "destroyed", &T::destroyed);
};

template <> struct glz::meta<RawEmailQueryResponse>
{
    using T = RawEmailQueryResponse;

    static constexpr auto value =
        glz::object("accountId", &T::accountId, "queryState", &T::queryState, "canCalculateChanges",
                    &T::canCalculateChanges, "position", &T::position, "ids", &T::ids, "total",
                    &T::total, "limit", &T::limit);
};

template <> struct glz::meta<RawAddedItem>
{
    using T = RawAddedItem;

    static constexpr auto value = glz::object("id", &T::id, "index", &T::index);
};

template <> struct glz::meta<RawEmailQueryChangesResponse>
{
    using T = RawEmailQueryChangesResponse;

    static constexpr auto value =
        glz::object("accountId", &T::accountId, "oldQueryState", &T::oldQueryState, "newQueryState",
                    &T::newQueryState, "added", &T::added, "removed", &T::removed, "hasMoreChanges",
                    &T::hasMoreChanges, "total", &T::total);
};

namespace javelin::jmap::api
{

    namespace
    {

        template <typename T> [[nodiscard]] std::optional<std::string> serializeMethod(const T& raw)
        {
            std::string buffer;
            const auto writeError = glz::write_json(raw, buffer);
            if (writeError)
            {
                return std::nullopt;
            }

            return buffer;
        }

        template <typename T> [[nodiscard]] ParsedEnvelope<T> parseMethod(std::string_view json)
        {
            std::string buffer{json};
            T raw{};
            const auto readError =
                glz::read<glz::opts{.error_on_unknown_keys = false}>(raw, buffer);
            if (readError)
            {
                return {
                    .value = std::nullopt,
                    .error = glz::format_error(readError, buffer),
                };
            }

            return {
                .value = std::move(raw),
                .error = std::nullopt,
            };
        }

        template <typename Entity, typename Convert>
        [[nodiscard]] ParsedEnvelope<std::vector<Entity>>
        convertEntities(const std::vector<glz::generic>& rawEntities, Convert&& convert)
        {
            std::vector<Entity> entities;
            entities.reserve(rawEntities.size());

            for (const auto& rawEntity : rawEntities)
            {
                std::string encoded;
                const auto writeError = glz::write_json(rawEntity, encoded);
                if (writeError)
                {
                    return {
                        .value = std::nullopt,
                        .error = "Failed to serialize typed JMAP entity",
                    };
                }

                const auto parsed = std::forward<Convert>(convert)(encoded);
                if (!parsed.ok())
                {
                    return {
                        .value = std::nullopt,
                        .error = parsed.error,
                    };
                }

                entities.push_back(std::move(*parsed.value));
            }

            return {
                .value = std::move(entities),
                .error = std::nullopt,
            };
        }

        [[nodiscard]] EmailContentBodyPart convertBodyPart(const RawEmailContentBodyPart& part)
        {
            return EmailContentBodyPart{
                .partId = part.partId,
                .blobId = part.blobId,
                .size = part.size,
                .name = part.name,
                .type = part.type,
                .charset = part.charset,
                .disposition = part.disposition,
                .cid = part.cid,
            };
        }

        [[nodiscard]] EmailContent convertEmailContent(RawEmailContent raw)
        {
            EmailContent content{
                .id = std::move(raw.id),
                .textBody = {},
                .htmlBody = {},
                .attachments = {},
                .bodyValues = {},
            };

            content.textBody.reserve(raw.textBody.size());
            for (const auto& part : raw.textBody)
            {
                content.textBody.push_back(convertBodyPart(part));
            }

            content.htmlBody.reserve(raw.htmlBody.size());
            for (const auto& part : raw.htmlBody)
            {
                content.htmlBody.push_back(convertBodyPart(part));
            }

            content.attachments.reserve(raw.attachments.size());
            for (const auto& part : raw.attachments)
            {
                content.attachments.push_back(convertBodyPart(part));
            }

            for (auto& [partId, bodyValue] : raw.bodyValues)
            {
                content.bodyValues.emplace(std::move(partId),
                                           EmailBodyValue{
                                               .isEncodingProblem = bodyValue.isEncodingProblem,
                                               .isTruncated = bodyValue.isTruncated,
                                               .value = std::move(bodyValue.value),
                                           });
            }

            return content;
        }

        [[nodiscard]] RawEmailBodyPartCreate
        convertEmailBodyPartCreate(const EmailBodyPartCreate& part)
        {
            std::optional<std::vector<RawEmailBodyPartCreate>> subParts = std::nullopt;
            if (part.subParts.has_value())
            {
                std::vector<RawEmailBodyPartCreate> converted;
                converted.reserve(part.subParts->size());
                for (const auto& subPart : *part.subParts)
                {
                    converted.push_back(convertEmailBodyPartCreate(subPart));
                }
                subParts = std::move(converted);
            }

            return RawEmailBodyPartCreate{
                .partId = part.partId,
                .blobId = part.blobId,
                .type = part.type,
                .name = part.name,
                .disposition = part.disposition,
                .cid = part.cid,
                .subParts = std::move(subParts),
            };
        }

    } // namespace

    std::optional<std::string> serializeGetRequest(const GetRequest& request)
    {
        return serializeMethod(RawGetRequest{
            .accountId = request.accountId,
            .ids = request.ids,
            .idsReference = request.idsReference,
            .properties = request.properties,
        });
    }

    std::optional<std::string> serializeChangesRequest(const ChangesRequest& request)
    {
        return serializeMethod(RawChangesRequest{
            .accountId = request.accountId,
            .sinceState = request.sinceState,
            .maxChanges = request.maxChanges,
        });
    }

    std::optional<std::string> serializeIdentitySetRequest(const IdentitySetRequest& request)
    {
        std::optional<std::unordered_map<std::string, RawIdentitySetCreate>> create;
        if (!request.create.empty())
        {
            std::unordered_map<std::string, RawIdentitySetCreate> values;
            values.reserve(request.create.size());
            for (const auto& [creationId, identity] : request.create)
            {
                values.emplace(creationId, RawIdentitySetCreate{
                                               .name = identity.name,
                                               .email = identity.email,
                                               .replyTo = identity.replyTo,
                                               .bcc = identity.bcc,
                                               .textSignature = identity.textSignature,
                                               .htmlSignature = identity.htmlSignature,
                                           });
            }
            create = std::move(values);
        }

        std::optional<std::unordered_map<std::string, RawIdentitySetUpdate>> update;
        if (!request.update.empty())
        {
            std::unordered_map<std::string, RawIdentitySetUpdate> values;
            values.reserve(request.update.size());
            for (const auto& [identityId, identity] : request.update)
            {
                values.emplace(identityId, RawIdentitySetUpdate{
                                               .name = identity.name,
                                               .replyTo = identity.replyTo,
                                               .bcc = identity.bcc,
                                               .textSignature = identity.textSignature,
                                               .htmlSignature = identity.htmlSignature,
                                           });
            }
            update = std::move(values);
        }

        return serializeMethod(RawIdentitySetRequest{
            .accountId = request.accountId,
            .ifInState = request.ifInState,
            .create = std::move(create),
            .update = std::move(update),
            .destroy = request.destroy.empty()
                           ? std::nullopt
                           : std::optional<std::vector<std::string>>{request.destroy},
        });
    }

    [[nodiscard]] RawEmailQueryFilter toRawEmailQueryFilter(const EmailQueryFilter& filter)
    {
        std::vector<RawEmailQueryFilter> conditions;
        conditions.reserve(filter.conditions.size());
        for (const auto& condition : filter.conditions)
        {
            conditions.push_back(toRawEmailQueryFilter(condition));
        }

        return RawEmailQueryFilter{
            .operatorName = filter.operatorName,
            .conditions = conditions.empty() ? std::nullopt : std::optional{std::move(conditions)},
            .inMailbox = filter.inMailbox,
            .text = filter.text,
            .from = filter.from,
            .to = filter.to,
            .cc = filter.cc,
            .bcc = filter.bcc,
            .subject = filter.subject,
            .body = filter.body,
            .hasKeyword = filter.hasKeyword,
            .notKeyword = filter.notKeyword,
            .hasAttachment = filter.hasAttachment,
        };
    }

    std::optional<std::string> serializeEmailQueryRequest(const EmailQueryRequest& request)
    {
        std::vector<RawEmailQuerySort> sort;
        sort.reserve(request.sort.size());
        for (const auto& comparator : request.sort)
        {
            sort.push_back(RawEmailQuerySort{
                .property = comparator.property,
                .isAscending = comparator.isAscending,
            });
        }

        return serializeMethod(RawEmailQueryRequest{
            .accountId = request.accountId,
            .filter =
                request.filter.has_value()
                    ? std::optional<RawEmailQueryFilter>{toRawEmailQueryFilter(*request.filter)}
                    : std::nullopt,
            .sort = std::move(sort),
            .position = request.position,
            .anchor = request.anchor,
            .anchorOffset = request.anchor.has_value()
                                ? std::optional<std::int64_t>{request.anchorOffset}
                                : std::nullopt,
            .limit = request.limit,
            .collapseThreads = request.collapseThreads,
            .calculateTotal = request.calculateTotal,
        });
    }

    std::optional<std::string>
    serializeEmailQueryChangesRequest(const EmailQueryChangesRequest& request)
    {
        std::vector<RawEmailQuerySort> sort;
        sort.reserve(request.sort.size());
        for (const auto& comparator : request.sort)
        {
            sort.push_back(RawEmailQuerySort{
                .property = comparator.property,
                .isAscending = comparator.isAscending,
            });
        }

        return serializeMethod(RawEmailQueryChangesRequest{
            .accountId = request.accountId,
            .sinceQueryState = request.sinceQueryState,
            .maxChanges = request.maxChanges,
            .upToId = request.upToId,
            .filter =
                request.filter.has_value()
                    ? std::optional<RawEmailQueryFilter>{toRawEmailQueryFilter(*request.filter)}
                    : std::nullopt,
            .sort = std::move(sort),
            .collapseThreads = request.collapseThreads,
            .calculateTotal = request.calculateTotal,
        });
    }

    std::optional<std::string>
    serializeEmailContentGetRequest(const EmailContentGetRequest& request)
    {
        return serializeMethod(RawEmailContentGetRequest{
            .accountId = request.accountId,
            .ids = request.ids,
            .properties = request.properties,
            .bodyProperties = request.bodyProperties,
            .fetchTextBodyValues = request.fetchTextBodyValues,
            .fetchHTMLBodyValues = request.fetchHTMLBodyValues,
            .fetchAllBodyValues = request.fetchAllBodyValues,
            .maxBodyValueBytes = request.maxBodyValueBytes,
        });
    }

    std::optional<std::string> serializeEmailSetRequest(const EmailSetRequest& request)
    {
        std::optional<std::unordered_map<std::string, RawEmailSetCreate>> rawCreate = std::nullopt;
        if (!request.create.empty())
        {
            std::unordered_map<std::string, RawEmailSetCreate> converted;
            converted.reserve(request.create.size());
            for (const auto& [creationId, create] : request.create)
            {
                std::unordered_map<std::string, RawEmailBodyValueCreate> bodyValues;
                bodyValues.reserve(create.bodyValues.size());
                for (const auto& [partId, bodyValue] : create.bodyValues)
                {
                    bodyValues.emplace(partId, RawEmailBodyValueCreate{
                                                   .value = bodyValue.value,
                                                   .isTruncated = bodyValue.isTruncated,
                                               });
                }

                converted.emplace(
                    creationId,
                    RawEmailSetCreate{
                        .mailboxIds = create.mailboxIds,
                        .keywords = create.keywords,
                        .from = create.from,
                        .to = create.to,
                        .cc = create.cc,
                        .bcc = create.bcc,
                        .replyTo = create.replyTo,
                        .subject = create.subject,
                        .receivedAt = create.receivedAt,
                        .sentAt = create.sentAt,
                        .messageId = create.messageId,
                        .inReplyTo = create.inReplyTo,
                        .references = create.references,
                        .bodyStructure =
                            create.bodyStructure.has_value()
                                ? std::optional<RawEmailBodyPartCreate>{convertEmailBodyPartCreate(
                                      *create.bodyStructure)}
                                : std::nullopt,
                        .bodyValues = std::move(bodyValues),
                    });
            }
            rawCreate = std::move(converted);
        }

        std::unordered_map<std::string,
                           std::unordered_map<std::string, javelin::jmap::api::EmailPatchValue>>
            rawUpdates;
        rawUpdates.reserve(request.update.size());
        for (const auto& [emailId, update] : request.update)
        {
            rawUpdates.emplace(emailId, update.patch);
        }

        return serializeMethod(RawEmailSetRequest{
            .accountId = request.accountId,
            .ifInState = request.ifInState,
            .create = std::move(rawCreate),
            .update = std::move(rawUpdates),
            .destroy = request.destroy.empty()
                           ? std::nullopt
                           : std::optional<std::vector<std::string>>{request.destroy},
        });
    }

    std::optional<std::string>
    serializeEmailSubmissionSetRequest(const EmailSubmissionSetRequest& request)
    {
        std::unordered_map<std::string, RawEmailSubmissionCreate> create;
        create.reserve(request.create.size());
        for (const auto& [creationId, submission] : request.create)
        {
            create.emplace(
                creationId,
                RawEmailSubmissionCreate{
                    .identityId = submission.identityId,
                    .emailId = submission.emailId,
                    .envelope =
                        submission.envelope.has_value()
                            ? std::optional<RawEmailSubmissionEnvelope>{RawEmailSubmissionEnvelope{
                                  .mailFrom =
                                      submission.envelope->mailFrom.has_value()
                                          ? std::optional<RawEnvelopeAddress>{RawEnvelopeAddress{
                                                .email = submission.envelope->mailFrom->email,
                                            }}
                                          : std::nullopt,
                                  .rcptTo =
                                      [&submission]()
                                  {
                                      std::vector<RawEnvelopeAddress> recipients;
                                      recipients.reserve(submission.envelope->rcptTo.size());
                                      for (const auto& recipient : submission.envelope->rcptTo)
                                      {
                                          recipients.push_back(RawEnvelopeAddress{
                                              .email = recipient.email,
                                          });
                                      }
                                      return recipients;
                                  }(),
                              }}
                            : std::nullopt,
                });
        }

        return serializeMethod(RawEmailSubmissionSetRequest{
            .accountId = request.accountId,
            .create = std::move(create),
            .onSuccessUpdateEmail =
                request.onSuccessUpdateEmail.empty()
                    ? std::nullopt
                    : std::optional<std::unordered_map<
                          std::string,
                          std::unordered_map<
                              std::string, EmailSubmissionPatchValue>>>{request
                                                                            .onSuccessUpdateEmail},
        });
    }

    ParsedEnvelope<IdentityGetResponse> parseIdentityGetResponse(std::string_view json)
    {
        const auto parsed = parseMethod<RawIdentityGetResponse>(json);
        if (!parsed.ok())
        {
            return {
                .value = std::nullopt,
                .error = parsed.error,
            };
        }

        const auto list = convertEntities<javelin::jmap::domain::Identity>(
            parsed.value->list, [](const std::string& entityJson)
            { return javelin::jmap::domain::parseIdentity(entityJson); });
        if (!list.ok())
        {
            return {
                .value = std::nullopt,
                .error = list.error,
            };
        }

        return {
            .value =
                IdentityGetResponse{
                    .accountId = std::move(parsed.value->accountId),
                    .state = std::move(parsed.value->state),
                    .list = std::move(*list.value),
                    .notFound = std::move(parsed.value->notFound),
                },
            .error = std::nullopt,
        };
    }

    ParsedEnvelope<IdentityChangesResponse> parseIdentityChangesResponse(std::string_view json)
    {
        const auto parsed = parseMethod<RawChangesResponse>(json);
        if (!parsed.ok())
        {
            return {.value = std::nullopt, .error = parsed.error};
        }

        return {
            .value =
                IdentityChangesResponse{
                    .accountId = std::move(parsed.value->accountId),
                    .oldState = std::move(parsed.value->oldState),
                    .newState = std::move(parsed.value->newState),
                    .hasMoreChanges = parsed.value->hasMoreChanges,
                    .created = std::move(parsed.value->created),
                    .updated = std::move(parsed.value->updated),
                    .destroyed = std::move(parsed.value->destroyed),
                },
            .error = std::nullopt,
        };
    }

    ParsedEnvelope<IdentitySetResponse> parseIdentitySetResponse(std::string_view json)
    {
        const auto parsed = parseMethod<RawIdentitySetResponse>(json);
        if (!parsed.ok())
        {
            return {.value = std::nullopt, .error = parsed.error};
        }

        IdentitySetResponse response{
            .accountId = std::move(parsed.value->accountId),
            .oldState = parsed.value->oldState.value_or(std::string{}),
            .newState = std::move(parsed.value->newState),
            .created = {},
            .updated = {},
            .destroyed = parsed.value->destroyed.value_or(std::vector<std::string>{}),
            .notCreated = {},
            .notUpdated = {},
            .notDestroyed = {},
        };

        for (auto& [creationId, created] : parsed.value->created.value_or(
                 std::unordered_map<std::string, RawIdentitySetCreated>{}))
        {
            response.created.emplace(std::move(creationId), std::move(created.id));
        }
        for (const auto& [identityId, ignored] :
             parsed.value->updated.value_or(std::unordered_map<std::string, glz::generic>{}))
        {
            static_cast<void>(ignored);
            response.updated.push_back(identityId);
        }

        const auto appendErrors = [](auto& destination, auto source)
        {
            for (auto& [id, error] : source)
            {
                destination.emplace(std::move(id), IdentitySetError{
                                                       .type = std::move(error.type),
                                                       .description = std::move(error.description),
                                                       .properties = std::move(error.properties),
                                                   });
            }
        };
        appendErrors(response.notCreated,
                     parsed.value->notCreated.value_or(
                         std::unordered_map<std::string, RawIdentitySetError>{}));
        appendErrors(response.notUpdated,
                     parsed.value->notUpdated.value_or(
                         std::unordered_map<std::string, RawIdentitySetError>{}));
        appendErrors(response.notDestroyed,
                     parsed.value->notDestroyed.value_or(
                         std::unordered_map<std::string, RawIdentitySetError>{}));

        return {.value = std::move(response), .error = std::nullopt};
    }

    ParsedEnvelope<MailboxGetResponse> parseMailboxGetResponse(std::string_view json)
    {
        const auto parsed = parseMethod<RawMailboxGetResponse>(json);
        if (!parsed.ok())
        {
            return {
                .value = std::nullopt,
                .error = parsed.error,
            };
        }

        const auto list = convertEntities<javelin::jmap::domain::Mailbox>(
            parsed.value->list, [](const std::string& entityJson)
            { return javelin::jmap::domain::parseMailbox(entityJson); });
        if (!list.ok())
        {
            return {
                .value = std::nullopt,
                .error = list.error,
            };
        }

        return {
            .value =
                MailboxGetResponse{
                    .accountId = std::move(parsed.value->accountId),
                    .state = std::move(parsed.value->state),
                    .list = std::move(*list.value),
                    .notFound = std::move(parsed.value->notFound),
                },
            .error = std::nullopt,
        };
    }

    ParsedEnvelope<EmailGetResponse> parseEmailGetResponse(std::string_view json)
    {
        const auto parsed = parseMethod<RawEmailGetResponse>(json);
        if (!parsed.ok())
        {
            return {
                .value = std::nullopt,
                .error = parsed.error,
            };
        }

        const auto list = convertEntities<javelin::jmap::domain::Email>(
            parsed.value->list, [](const std::string& entityJson)
            { return javelin::jmap::domain::parseEmail(entityJson); });
        if (!list.ok())
        {
            return {
                .value = std::nullopt,
                .error = list.error,
            };
        }

        return {
            .value =
                EmailGetResponse{
                    .accountId = std::move(parsed.value->accountId),
                    .state = std::move(parsed.value->state),
                    .list = std::move(*list.value),
                    .notFound = std::move(parsed.value->notFound),
                },
            .error = std::nullopt,
        };
    }

    ParsedEnvelope<ThreadGetResponse> parseThreadGetResponse(std::string_view json)
    {
        const auto parsed = parseMethod<RawThreadGetResponse>(json);
        if (!parsed.ok())
        {
            return {
                .value = std::nullopt,
                .error = parsed.error,
            };
        }

        const auto list = convertEntities<javelin::jmap::domain::Thread>(
            parsed.value->list, [](const std::string& entityJson)
            { return javelin::jmap::domain::parseThread(entityJson); });
        if (!list.ok())
        {
            return {
                .value = std::nullopt,
                .error = list.error,
            };
        }

        return {
            .value =
                ThreadGetResponse{
                    .accountId = std::move(parsed.value->accountId),
                    .state = std::move(parsed.value->state),
                    .list = std::move(*list.value),
                    .notFound = std::move(parsed.value->notFound),
                },
            .error = std::nullopt,
        };
    }

    ParsedEnvelope<EmailQueryResponse> parseEmailQueryResponse(std::string_view json)
    {
        const auto parsed = parseMethod<RawEmailQueryResponse>(json);
        if (!parsed.ok())
        {
            return {
                .value = std::nullopt,
                .error = parsed.error,
            };
        }

        return {
            .value =
                EmailQueryResponse{
                    .accountId = std::move(parsed.value->accountId),
                    .queryState = std::move(parsed.value->queryState),
                    .canCalculateChanges = parsed.value->canCalculateChanges,
                    .position = parsed.value->position,
                    .ids = std::move(parsed.value->ids),
                    .total = parsed.value->total,
                    .limit = parsed.value->limit,
                },
            .error = std::nullopt,
        };
    }

    ParsedEnvelope<EmailQueryChangesResponse> parseEmailQueryChangesResponse(std::string_view json)
    {
        const auto parsed = parseMethod<RawEmailQueryChangesResponse>(json);
        if (!parsed.ok())
        {
            return {
                .value = std::nullopt,
                .error = parsed.error,
            };
        }

        std::vector<AddedItem> added;
        added.reserve(parsed.value->added.size());
        for (const auto& item : parsed.value->added)
        {
            added.push_back(AddedItem{
                .id = item.id,
                .index = item.index,
            });
        }

        return {
            .value =
                EmailQueryChangesResponse{
                    .accountId = std::move(parsed.value->accountId),
                    .oldQueryState = std::move(parsed.value->oldQueryState),
                    .newQueryState = std::move(parsed.value->newQueryState),
                    .added = std::move(added),
                    .removed = std::move(parsed.value->removed),
                    .hasMoreChanges = parsed.value->hasMoreChanges,
                    .total = parsed.value->total,
                },
            .error = std::nullopt,
        };
    }

    ParsedEnvelope<EmailContentGetResponse> parseEmailContentGetResponse(std::string_view json)
    {
        const auto parsed = parseMethod<RawEmailContentGetResponse>(json);
        if (!parsed.ok())
        {
            return {
                .value = std::nullopt,
                .error = parsed.error,
            };
        }

        EmailContentGetResponse response{
            .accountId = std::move(parsed.value->accountId),
            .state = std::move(parsed.value->state),
            .list = {},
            .notFound = std::move(parsed.value->notFound),
        };
        response.list.reserve(parsed.value->list.size());
        for (auto& rawContent : parsed.value->list)
        {
            response.list.push_back(convertEmailContent(rawContent));
        }

        return {
            .value = std::move(response),
            .error = std::nullopt,
        };
    }

    ParsedEnvelope<EmailSetResponse> parseEmailSetResponse(std::string_view json)
    {
        const auto parsed = parseMethod<RawEmailSetResponse>(json);
        if (!parsed.ok())
        {
            return {
                .value = std::nullopt,
                .error = parsed.error,
            };
        }

        std::unordered_map<std::string, EmailSetCreated> created;
        const auto& rawCreated =
            parsed.value->created.value_or(std::unordered_map<std::string, RawEmailSetCreated>{});
        for (const auto& [creationId, value] : rawCreated)
        {
            created.emplace(creationId, EmailSetCreated{
                                            .id = value.id,
                                            .blobId = value.blobId,
                                            .threadId = value.threadId,
                                            .size = value.size,
                                        });
        }

        std::vector<std::string> updated;
        const auto& rawUpdated =
            parsed.value->updated.value_or(std::unordered_map<std::string, glz::generic>{});
        updated.reserve(rawUpdated.size());
        for (const auto& [emailId, ignored] : rawUpdated)
        {
            static_cast<void>(ignored);
            updated.push_back(emailId);
        }

        std::vector<std::string> destroyed =
            parsed.value->destroyed.value_or(std::vector<std::string>{});

        std::vector<std::string> notCreated;
        const auto& rawNotCreated =
            parsed.value->notCreated.value_or(std::unordered_map<std::string, glz::generic>{});
        notCreated.reserve(rawNotCreated.size());
        for (const auto& [creationId, ignored] : rawNotCreated)
        {
            static_cast<void>(ignored);
            notCreated.push_back(creationId);
        }

        std::vector<std::string> notUpdated;
        const auto& rawNotUpdated =
            parsed.value->notUpdated.value_or(std::unordered_map<std::string, glz::generic>{});
        notUpdated.reserve(rawNotUpdated.size());
        for (const auto& [emailId, ignored] : rawNotUpdated)
        {
            static_cast<void>(ignored);
            notUpdated.push_back(emailId);
        }

        std::vector<std::string> notDestroyed;
        const auto& rawNotDestroyed =
            parsed.value->notDestroyed.value_or(std::unordered_map<std::string, glz::generic>{});
        notDestroyed.reserve(rawNotDestroyed.size());
        for (const auto& [emailId, ignored] : rawNotDestroyed)
        {
            static_cast<void>(ignored);
            notDestroyed.push_back(emailId);
        }

        return {
            .value =
                EmailSetResponse{
                    .accountId = std::move(parsed.value->accountId),
                    .oldState = parsed.value->oldState.value_or(std::string{}),
                    .newState = std::move(parsed.value->newState),
                    .created = std::move(created),
                    .updated = std::move(updated),
                    .destroyed = std::move(destroyed),
                    .notCreated = std::move(notCreated),
                    .notUpdated = std::move(notUpdated),
                    .notDestroyed = std::move(notDestroyed),
                },
            .error = std::nullopt,
        };
    }

    ParsedEnvelope<EmailSubmissionSetResponse>
    parseEmailSubmissionSetResponse(std::string_view json)
    {
        const auto parsed = parseMethod<RawEmailSubmissionSetResponse>(json);
        if (!parsed.ok())
        {
            return {
                .value = std::nullopt,
                .error = parsed.error,
            };
        }

        std::unordered_map<std::string, EmailSubmissionCreated> created;
        const auto& rawCreated = parsed.value->created.value_or(
            std::unordered_map<std::string, RawEmailSubmissionCreated>{});
        for (const auto& [creationId, value] : rawCreated)
        {
            created.emplace(creationId, EmailSubmissionCreated{.id = value.id});
        }

        std::vector<std::string> notCreated;
        const auto& rawNotCreated =
            parsed.value->notCreated.value_or(std::unordered_map<std::string, glz::generic>{});
        notCreated.reserve(rawNotCreated.size());
        for (const auto& [creationId, ignored] : rawNotCreated)
        {
            static_cast<void>(ignored);
            notCreated.push_back(creationId);
        }

        return {
            .value =
                EmailSubmissionSetResponse{
                    .accountId = std::move(parsed.value->accountId),
                    .oldState = parsed.value->oldState.value_or(std::string{}),
                    .newState = std::move(parsed.value->newState),
                    .created = std::move(created),
                    .notCreated = std::move(notCreated),
                },
            .error = std::nullopt,
        };
    }

    ParsedEnvelope<ChangesResponse> parseChangesResponse(std::string_view json)
    {
        const auto parsed = parseMethod<RawChangesResponse>(json);
        if (!parsed.ok())
        {
            return {
                .value = std::nullopt,
                .error = parsed.error,
            };
        }

        return {
            .value =
                ChangesResponse{
                    .accountId = std::move(parsed.value->accountId),
                    .oldState = std::move(parsed.value->oldState),
                    .newState = std::move(parsed.value->newState),
                    .hasMoreChanges = parsed.value->hasMoreChanges,
                    .created = std::move(parsed.value->created),
                    .updated = std::move(parsed.value->updated),
                    .destroyed = std::move(parsed.value->destroyed),
                },
            .error = std::nullopt,
        };
    }

    ParsedEnvelope<EmailChangesResponse> parseEmailChangesResponse(std::string_view json)
    {
        const auto parsed = parseChangesResponse(json);
        if (!parsed.ok() || !parsed.value.has_value())
        {
            return {
                .value = std::nullopt,
                .error = parsed.error,
            };
        }

        return {
            .value =
                EmailChangesResponse{
                    .accountId = std::move(parsed.value->accountId),
                    .oldState = std::move(parsed.value->oldState),
                    .newState = std::move(parsed.value->newState),
                    .hasMoreChanges = parsed.value->hasMoreChanges,
                    .created = std::move(parsed.value->created),
                    .updated = std::move(parsed.value->updated),
                    .destroyed = std::move(parsed.value->destroyed),
                },
            .error = std::nullopt,
        };
    }

    ParsedEnvelope<MailboxChangesResponse> parseMailboxChangesResponse(std::string_view json)
    {
        const auto parsed = parseChangesResponse(json);
        if (!parsed.ok() || !parsed.value.has_value())
        {
            return {
                .value = std::nullopt,
                .error = parsed.error,
            };
        }

        return {
            .value =
                MailboxChangesResponse{
                    ChangesResponse{
                        .accountId = std::move(parsed.value->accountId),
                        .oldState = std::move(parsed.value->oldState),
                        .newState = std::move(parsed.value->newState),
                        .hasMoreChanges = parsed.value->hasMoreChanges,
                        .created = std::move(parsed.value->created),
                        .updated = std::move(parsed.value->updated),
                        .destroyed = std::move(parsed.value->destroyed),
                    },
                },
            .error = std::nullopt,
        };
    }

    std::optional<MethodRequest<IdentityGetResponse>> identityGet(const GetRequest& request)
    {
        const auto arguments = serializeGetRequest(request);
        if (!arguments.has_value())
        {
            return std::nullopt;
        }

        return MethodRequest<IdentityGetResponse>{
            .name = "Identity/get",
            .arguments = *arguments,
        };
    }

    std::optional<MethodRequest<IdentityChangesResponse>>
    identityChanges(const ChangesRequest& request)
    {
        const auto arguments = serializeChangesRequest(request);
        if (!arguments.has_value())
            return std::nullopt;
        return MethodRequest<IdentityChangesResponse>{
            .name = "Identity/changes",
            .arguments = *arguments,
        };
    }

    std::optional<MethodRequest<IdentitySetResponse>> identitySet(const IdentitySetRequest& request)
    {
        const auto arguments = serializeIdentitySetRequest(request);
        if (!arguments.has_value())
            return std::nullopt;
        return MethodRequest<IdentitySetResponse>{
            .name = "Identity/set",
            .arguments = *arguments,
        };
    }

    std::optional<MethodRequest<MailboxGetResponse>> mailboxGet(const GetRequest& request)
    {
        const auto arguments = serializeGetRequest(request);
        if (!arguments.has_value())
        {
            return std::nullopt;
        }

        return MethodRequest<MailboxGetResponse>{
            .name = "Mailbox/get",
            .arguments = *arguments,
        };
    }

    std::optional<MethodRequest<EmailGetResponse>> emailGet(const GetRequest& request)
    {
        const auto arguments = serializeGetRequest(request);
        if (!arguments.has_value())
        {
            return std::nullopt;
        }

        return MethodRequest<EmailGetResponse>{
            .name = "Email/get",
            .arguments = *arguments,
        };
    }

    std::optional<MethodRequest<ThreadGetResponse>> threadGet(const GetRequest& request)
    {
        const auto arguments = serializeGetRequest(request);
        if (!arguments.has_value())
        {
            return std::nullopt;
        }

        return MethodRequest<ThreadGetResponse>{
            .name = "Thread/get",
            .arguments = *arguments,
        };
    }

    std::optional<MethodRequest<EmailQueryResponse>> emailQuery(const EmailQueryRequest& request)
    {
        const auto arguments = serializeEmailQueryRequest(request);
        if (!arguments.has_value())
        {
            return std::nullopt;
        }

        return MethodRequest<EmailQueryResponse>{
            .name = "Email/query",
            .arguments = *arguments,
        };
    }

    std::optional<MethodRequest<EmailQueryChangesResponse>>
    emailQueryChanges(const EmailQueryChangesRequest& request)
    {
        const auto arguments = serializeEmailQueryChangesRequest(request);
        if (!arguments.has_value())
        {
            return std::nullopt;
        }

        return MethodRequest<EmailQueryChangesResponse>{
            .name = "Email/queryChanges",
            .arguments = *arguments,
        };
    }

    std::optional<MethodRequest<EmailChangesResponse>> emailChanges(const ChangesRequest& request)
    {
        const auto arguments = serializeChangesRequest(request);
        if (!arguments.has_value())
        {
            return std::nullopt;
        }

        return MethodRequest<EmailChangesResponse>{
            .name = "Email/changes",
            .arguments = *arguments,
        };
    }

    std::optional<MethodRequest<MailboxChangesResponse>>
    mailboxChanges(const ChangesRequest& request)
    {
        const auto arguments = serializeChangesRequest(request);
        if (!arguments.has_value())
        {
            return std::nullopt;
        }

        return MethodRequest<MailboxChangesResponse>{
            .name = "Mailbox/changes",
            .arguments = *arguments,
        };
    }

    std::optional<MethodRequest<EmailContentGetResponse>>
    emailContentGet(const EmailContentGetRequest& request)
    {
        const auto arguments = serializeEmailContentGetRequest(request);
        if (!arguments.has_value())
        {
            return std::nullopt;
        }

        return MethodRequest<EmailContentGetResponse>{
            .name = "Email/get",
            .arguments = *arguments,
        };
    }

    std::optional<MethodRequest<EmailSetResponse>> emailSet(const EmailSetRequest& request)
    {
        const auto arguments = serializeEmailSetRequest(request);
        if (!arguments.has_value())
        {
            return std::nullopt;
        }

        return MethodRequest<EmailSetResponse>{
            .name = "Email/set",
            .arguments = *arguments,
        };
    }

    std::optional<MethodRequest<EmailSubmissionSetResponse>>
    emailSubmissionSet(const EmailSubmissionSetRequest& request)
    {
        const auto arguments = serializeEmailSubmissionSetRequest(request);
        if (!arguments.has_value())
        {
            return std::nullopt;
        }

        return MethodRequest<EmailSubmissionSetResponse>{
            .name = "EmailSubmission/set",
            .arguments = *arguments,
        };
    }

} // namespace javelin::jmap::api
