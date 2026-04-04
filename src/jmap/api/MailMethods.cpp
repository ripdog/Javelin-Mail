#include "jmap/api/MailMethods.h"

#include "jmap/domain/MailEntityParsers.h"

#include <glaze/glaze.hpp>

namespace
{

    struct RawGetRequest
    {
        std::string accountId;
        std::optional<std::vector<std::string>> ids;
        std::optional<std::vector<std::string>> properties;
    };

    struct RawMailboxGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<glz::json_t> list;
        std::vector<std::string> notFound;
    };

    struct RawEmailGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<glz::json_t> list;
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
        std::optional<std::string> inMailbox;
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
        std::optional<std::uint64_t> limit;
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
        std::uint64_t maxBodyValueBytes = 0;
    };

    struct RawEmailContentGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<RawEmailContent> list;
        std::vector<std::string> notFound;
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
    };

} // namespace

template <> struct glz::meta<RawGetRequest>
{
    using T = RawGetRequest;

    static constexpr auto value =
        glz::object("accountId", &T::accountId, "ids", &T::ids, "properties", &T::properties);
};

template <> struct glz::meta<RawMailboxGetResponse>
{
    using T = RawMailboxGetResponse;

    static constexpr auto value = glz::object("accountId", &T::accountId, "state", &T::state,
                                              "list", &T::list, "notFound", &T::notFound);
};

template <> struct glz::meta<RawEmailGetResponse>
{
    using T = RawEmailGetResponse;

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

    static constexpr auto value = glz::object("inMailbox", &T::inMailbox);
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

    static constexpr auto value =
        glz::object("accountId", &T::accountId, "filter", &T::filter, "sort", &T::sort, "position",
                    &T::position, "limit", &T::limit, "collapseThreads", &T::collapseThreads,
                    "calculateTotal", &T::calculateTotal);
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

    static constexpr auto value = glz::object(
        "accountId", &T::accountId, "queryState", &T::queryState, "canCalculateChanges",
        &T::canCalculateChanges, "position", &T::position, "ids", &T::ids, "total", &T::total);
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
        convertEntities(const std::vector<glz::json_t>& rawEntities, Convert&& convert)
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

    } // namespace

    std::optional<std::string> serializeGetRequest(const GetRequest& request)
    {
        return serializeMethod(RawGetRequest{
            .accountId = request.accountId,
            .ids = request.ids,
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
            .filter = request.filter.has_value()
                          ? std::optional<RawEmailQueryFilter>{RawEmailQueryFilter{
                                .inMailbox = request.filter->inMailbox}}
                          : std::nullopt,
            .sort = std::move(sort),
            .position = request.position,
            .limit = request.limit,
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

} // namespace javelin::jmap::api
