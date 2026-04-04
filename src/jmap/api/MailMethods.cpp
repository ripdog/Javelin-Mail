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

template <> struct glz::meta<RawChangesResponse>
{
    using T = RawChangesResponse;

    static constexpr auto value =
        glz::object("accountId", &T::accountId, "oldState", &T::oldState, "newState", &T::newState,
                    "hasMoreChanges", &T::hasMoreChanges, "created", &T::created, "updated",
                    &T::updated, "destroyed", &T::destroyed);
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
            const auto readError = glz::read_json(raw, buffer);
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
