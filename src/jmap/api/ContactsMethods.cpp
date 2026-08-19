#include "jmap/api/ContactsMethods.h"

#include <glaze/glaze.hpp>

#include <utility>

namespace javelin::jmap::api::detail
{
    struct RawContactCardSummary
    {
        std::string id;
        std::string uid;
        std::string kind;
    };

    struct RawContactCardGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<glz::generic> list;
        std::vector<std::string> notFound;
    };

    struct RawDocumentSetRequest
    {
        std::string accountId;
        std::optional<std::string> ifInState;
        std::unordered_map<std::string, glz::raw_json> create;
        std::unordered_map<std::string, glz::raw_json> update;
        std::vector<std::string> destroy;
    };

    struct RawAddressBookSetRequest : RawDocumentSetRequest
    {
        bool onDestroyRemoveContents = false;
        std::optional<std::string> onSuccessSetIsDefault;
    };

    struct RawContactCardCopyRequest
    {
        std::string fromAccountId;
        std::string accountId;
        std::optional<std::string> ifFromInState;
        std::optional<std::string> ifInState;
        std::unordered_map<std::string, glz::raw_json> create;
        bool onSuccessDestroyOriginal = false;
        std::optional<std::string> destroyFromIfInState;
    };

    struct RawAddressBookCreate
    {
        std::string name;
        std::optional<std::string> description;
        std::uint32_t sortOrder = 0;
        bool isSubscribed = true;
        std::optional<std::unordered_map<std::string, AddressBookRights>> shareWith;
    };

    struct RawSetResult
    {
        std::string accountId;
        std::optional<std::string> oldState;
        std::string newState;
        std::optional<std::unordered_map<std::string, glz::generic>> created;
        std::optional<std::unordered_map<std::string, glz::generic>> updated;
        std::optional<std::vector<std::string>> destroyed;
        std::optional<std::unordered_map<std::string, glz::generic>> notCreated;
        std::optional<std::unordered_map<std::string, glz::generic>> notUpdated;
        std::optional<std::unordered_map<std::string, glz::generic>> notDestroyed;
    };
} // namespace javelin::jmap::api::detail

template <> struct glz::meta<javelin::jmap::api::AddressBookRights>
{
    using T = javelin::jmap::api::AddressBookRights;
    static constexpr auto value = glz::object("mayRead", &T::mayRead, "mayWrite", &T::mayWrite,
                                              "mayShare", &T::mayShare, "mayDelete", &T::mayDelete);
};

template <> struct glz::meta<javelin::jmap::api::AddressBook>
{
    using T = javelin::jmap::api::AddressBook;
    static constexpr auto value =
        glz::object("id", &T::id, "name", &T::name, "description", &T::description, "sortOrder",
                    &T::sortOrder, "isDefault", &T::isDefault, "isSubscribed", &T::isSubscribed,
                    "shareWith", &T::shareWith, "myRights", &T::myRights);
};

template <> struct glz::meta<javelin::jmap::api::AddressBookGetResponse>
{
    using T = javelin::jmap::api::AddressBookGetResponse;
    static constexpr auto value = glz::object("accountId", &T::accountId, "state", &T::state,
                                              "list", &T::list, "notFound", &T::notFound);
};

template <> struct glz::meta<javelin::jmap::api::detail::RawContactCardSummary>
{
    using T = javelin::jmap::api::detail::RawContactCardSummary;
    static constexpr auto value = glz::object("id", &T::id, "uid", &T::uid, "kind", &T::kind);
};

template <> struct glz::meta<javelin::jmap::api::detail::RawContactCardGetResponse>
{
    using T = javelin::jmap::api::detail::RawContactCardGetResponse;
    static constexpr auto value = glz::object("accountId", &T::accountId, "state", &T::state,
                                              "list", &T::list, "notFound", &T::notFound);
};

template <> struct glz::meta<javelin::jmap::api::ContactCardQueryFilter>
{
    using T = javelin::jmap::api::ContactCardQueryFilter;
    static constexpr auto value =
        glz::object("operator", &T::operatorName, "conditions", &T::conditions, "inAddressBook",
                    &T::inAddressBook, "uid", &T::uid, "hasMember", &T::hasMember, "kind", &T::kind,
                    "createdBefore", &T::createdBefore, "createdAfter", &T::createdAfter,
                    "updatedBefore", &T::updatedBefore, "updatedAfter", &T::updatedAfter, "text",
                    &T::text, "name", &T::name, "name/given", &T::nameGiven, "name/surname",
                    &T::nameSurname, "name/surname2", &T::nameSurname2, "nickname", &T::nickname,
                    "organization", &T::organization, "email", &T::email, "phone", &T::phone,
                    "onlineService", &T::onlineService, "address", &T::address, "note", &T::note);
};

template <> struct glz::meta<javelin::jmap::api::ContactCardComparator>
{
    using T = javelin::jmap::api::ContactCardComparator;
    static constexpr auto value = glz::object("property", &T::property, "isAscending",
                                              &T::isAscending, "collation", &T::collation);
};

template <> struct glz::meta<javelin::jmap::api::ContactCardQueryRequest>
{
    using T = javelin::jmap::api::ContactCardQueryRequest;
    static constexpr auto value =
        glz::object("accountId", &T::accountId, "filter", &T::filter, "sort", &T::sort, "position",
                    &T::position, "anchor", &T::anchor, "anchorOffset", &T::anchorOffset, "limit",
                    &T::limit, "calculateTotal", &T::calculateTotal);
};

template <> struct glz::meta<javelin::jmap::api::detail::RawDocumentSetRequest>
{
    using T = javelin::jmap::api::detail::RawDocumentSetRequest;
    static constexpr auto value =
        glz::object("accountId", &T::accountId, "ifInState", &T::ifInState, "create", &T::create,
                    "update", &T::update, "destroy", &T::destroy);
};

template <> struct glz::meta<javelin::jmap::api::detail::RawAddressBookSetRequest>
{
    using T = javelin::jmap::api::detail::RawAddressBookSetRequest;
    static constexpr auto value = glz::object(
        "accountId", &T::accountId, "ifInState", &T::ifInState, "create", &T::create, "update",
        &T::update, "destroy", &T::destroy, "onDestroyRemoveContents", &T::onDestroyRemoveContents,
        "onSuccessSetIsDefault", &T::onSuccessSetIsDefault);
};

template <> struct glz::meta<javelin::jmap::api::detail::RawContactCardCopyRequest>
{
    using T = javelin::jmap::api::detail::RawContactCardCopyRequest;
    static constexpr auto value =
        glz::object("fromAccountId", &T::fromAccountId, "accountId", &T::accountId, "ifFromInState",
                    &T::ifFromInState, "ifInState", &T::ifInState, "create", &T::create,
                    "onSuccessDestroyOriginal", &T::onSuccessDestroyOriginal,
                    "destroyFromIfInState", &T::destroyFromIfInState);
};

template <> struct glz::meta<javelin::jmap::api::detail::RawAddressBookCreate>
{
    using T = javelin::jmap::api::detail::RawAddressBookCreate;
    static constexpr auto value =
        glz::object("name", &T::name, "description", &T::description, "sortOrder", &T::sortOrder,
                    "isSubscribed", &T::isSubscribed, "shareWith", &T::shareWith);
};

template <> struct glz::meta<javelin::jmap::api::detail::RawSetResult>
{
    using T = javelin::jmap::api::detail::RawSetResult;
    static constexpr auto value = glz::object(
        "accountId", &T::accountId, "oldState", &T::oldState, "newState", &T::newState, "created",
        &T::created, "updated", &T::updated, "destroyed", &T::destroyed, "notCreated",
        &T::notCreated, "notUpdated", &T::notUpdated, "notDestroyed", &T::notDestroyed);
};

namespace javelin::jmap::api
{
    namespace
    {
        template <typename T> [[nodiscard]] std::optional<std::string> serialize(const T& value)
        {
            std::string json;
            if (glz::write_json(value, json))
            {
                return std::nullopt;
            }
            return json;
        }

        template <typename T> [[nodiscard]] ParsedEnvelope<T> parse(std::string_view json)
        {
            std::string buffer{json};
            T value;
            const auto error = glz::read<glz::opts{.error_on_unknown_keys = false}>(value, buffer);
            if (error)
            {
                return {.value = std::nullopt, .error = glz::format_error(error, buffer)};
            }
            return {.value = std::move(value), .error = std::nullopt};
        }

        [[nodiscard]] std::unordered_map<std::string, glz::raw_json>
        rawDocuments(const std::unordered_map<std::string, ContactDocument>& documents)
        {
            std::unordered_map<std::string, glz::raw_json> result;
            result.reserve(documents.size());
            for (const auto& [id, document] : documents)
            {
                result.emplace(id, document.json);
            }
            return result;
        }

        [[nodiscard]] std::unordered_map<std::string, ContactDocument>
        documents(const std::unordered_map<std::string, glz::generic>& raw)
        {
            std::unordered_map<std::string, ContactDocument> result;
            result.reserve(raw.size());
            for (const auto& [id, value] : raw)
            {
                std::string json;
                if (!glz::write_json(value, json))
                {
                    result.emplace(id, ContactDocument{.json = std::move(json)});
                }
            }
            return result;
        }

        [[nodiscard]] std::unordered_map<std::string, std::optional<ContactDocument>>
        optionalDocuments(const std::unordered_map<std::string, glz::generic>& raw)
        {
            std::unordered_map<std::string, std::optional<ContactDocument>> result;
            result.reserve(raw.size());
            for (const auto& [id, value] : raw)
            {
                if (value.holds<glz::generic::null_t>())
                {
                    result.emplace(id, std::nullopt);
                    continue;
                }
                std::string json;
                if (!glz::write_json(value, json))
                    result.emplace(id, ContactDocument{.json = std::move(json)});
            }
            return result;
        }
    } // namespace

    std::optional<std::string>
    serializeContactCardQueryRequest(const ContactCardQueryRequest& request)
    {
        return serialize(request);
    }

    std::optional<std::string> serializeAddressBookSetRequest(const AddressBookSetRequest& request)
    {
        detail::RawAddressBookSetRequest raw;
        raw.accountId = request.accountId;
        raw.ifInState = request.ifInState;
        raw.create = rawDocuments(request.create);
        raw.update = rawDocuments(request.update);
        raw.destroy = request.destroy;
        raw.onDestroyRemoveContents = request.onDestroyRemoveContents;
        raw.onSuccessSetIsDefault = request.onSuccessSetIsDefault;
        return serialize(raw);
    }

    std::optional<std::string> serializeContactCardSetRequest(const ContactCardSetRequest& request)
    {
        return serialize(detail::RawDocumentSetRequest{
            .accountId = request.accountId,
            .ifInState = request.ifInState,
            .create = rawDocuments(request.create),
            .update = rawDocuments(request.update),
            .destroy = request.destroy,
        });
    }

    std::optional<std::string>
    serializeContactCardCopyRequest(const ContactCardCopyRequest& request)
    {
        return serialize(detail::RawContactCardCopyRequest{
            .fromAccountId = request.fromAccountId,
            .accountId = request.accountId,
            .ifFromInState = request.ifFromInState,
            .ifInState = request.ifInState,
            .create = rawDocuments(request.create),
            .onSuccessDestroyOriginal = request.onSuccessDestroyOriginal,
            .destroyFromIfInState = request.destroyFromIfInState,
        });
    }

    ContactDocument addressBookCreateDocument(const AddressBook& addressBook)
    {
        return {.json =
                    serialize(detail::RawAddressBookCreate{.name = addressBook.name,
                                                           .description = addressBook.description,
                                                           .sortOrder = addressBook.sortOrder,
                                                           .isSubscribed = addressBook.isSubscribed,
                                                           .shareWith = addressBook.shareWith})
                        .value_or("{}")};
    }

    ContactDocument addressBookUpdateDocument(const AddressBook& addressBook)
    {
        return addressBookCreateDocument(addressBook);
    }

    std::optional<std::string> serializeAddressBookDocument(const AddressBook& addressBook)
    {
        return serialize(addressBook);
    }

    ParsedEnvelope<AddressBook> parseAddressBookDocument(const std::string_view id,
                                                         const std::string_view json)
    {
        auto parsed = parse<AddressBook>(json);
        if (parsed.value.has_value())
            parsed.value->id = std::string{id};
        return parsed;
    }

    ParsedEnvelope<AddressBookGetResponse> parseAddressBookGetResponse(std::string_view json)
    {
        return parse<AddressBookGetResponse>(json);
    }

    ParsedEnvelope<ContactCardGetResponse> parseContactCardGetResponse(std::string_view json)
    {
        const auto raw = parse<detail::RawContactCardGetResponse>(json);
        if (!raw.ok())
        {
            return {.value = std::nullopt, .error = raw.error};
        }
        ContactCardGetResponse result{.accountId = raw.value->accountId,
                                      .state = raw.value->state,
                                      .list = {},
                                      .notFound = raw.value->notFound};
        result.list.reserve(raw.value->list.size());
        for (const auto& value : raw.value->list)
        {
            std::string document;
            if (glz::write_json(value, document))
            {
                return {.value = std::nullopt, .error = "Unable to preserve ContactCard JSON"};
            }
            detail::RawContactCardSummary summary;
            if (const auto error =
                    glz::read<glz::opts{.error_on_unknown_keys = false}>(summary, document))
            {
                return {.value = std::nullopt, .error = glz::format_error(error, document)};
            }
            result.list.push_back(ContactCard{.id = std::move(summary.id),
                                              .uid = std::move(summary.uid),
                                              .kind = std::move(summary.kind),
                                              .document = std::move(document)});
        }
        return {.value = std::move(result), .error = std::nullopt};
    }

    ParsedEnvelope<SetResult> parseContactsSetResponse(std::string_view json)
    {
        const auto raw = parse<detail::RawSetResult>(json);
        if (!raw.ok())
        {
            return {.value = std::nullopt, .error = raw.error};
        }
        return {.value = SetResult{.accountId = raw.value->accountId,
                                   .oldState = raw.value->oldState.value_or(std::string{}),
                                   .newState = raw.value->newState,
                                   .created = documents(raw.value->created.value_or(
                                       std::unordered_map<std::string, glz::generic>{})),
                                   .updated = optionalDocuments(raw.value->updated.value_or(
                                       std::unordered_map<std::string, glz::generic>{})),
                                   .destroyed =
                                       raw.value->destroyed.value_or(std::vector<std::string>{}),
                                   .notCreated = documents(raw.value->notCreated.value_or(
                                       std::unordered_map<std::string, glz::generic>{})),
                                   .notUpdated = documents(raw.value->notUpdated.value_or(
                                       std::unordered_map<std::string, glz::generic>{})),
                                   .notDestroyed = documents(raw.value->notDestroyed.value_or(
                                       std::unordered_map<std::string, glz::generic>{}))},
                .error = std::nullopt};
    }
} // namespace javelin::jmap::api
