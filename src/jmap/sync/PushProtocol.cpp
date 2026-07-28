#include "jmap/sync/PushProtocol.h"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <charconv>
#include <unordered_map>

namespace javelin::jmap::sync
{
    struct RawEventSourceStateChange
    {
        std::optional<std::string> type;
        std::optional<std::string> objectType;
        AccountTypeStateMap changed;
    };

    struct RawWebSocketStateChange
    {
        std::optional<std::string> type;
        AccountTypeStateMap changed;
        std::optional<std::string> pushState;
    };

    struct RawPushPing
    {
        std::optional<std::variant<unsigned int, std::string>> interval;
    };
} // namespace javelin::jmap::sync

template <> struct glz::meta<javelin::jmap::sync::RawEventSourceStateChange>
{
    using T = javelin::jmap::sync::RawEventSourceStateChange;
    static constexpr auto value =
        glz::object("type", &T::type, "@type", &T::objectType, "changed", &T::changed);
};

template <> struct glz::meta<javelin::jmap::sync::RawWebSocketStateChange>
{
    using T = javelin::jmap::sync::RawWebSocketStateChange;
    static constexpr auto value =
        glz::object("@type", &T::type, "changed", &T::changed, "pushState", &T::pushState);
};

template <> struct glz::meta<javelin::jmap::sync::RawPushPing>
{
    using T = javelin::jmap::sync::RawPushPing;
    static constexpr auto value = glz::object("interval", &T::interval);
};

namespace javelin::jmap::sync
{
    namespace
    {
        [[nodiscard]] StateChangeEvent
        makeStateChangeEvent(const StateChangeSubscription& subscription,
                             const std::string_view fallbackState, AccountTypeStateMap changed,
                             const std::optional<std::string>& pushedState)
        {
            StateChangeEvent event{
                .newState = pushedState.value_or(std::string{fallbackState}),
                .changedTypes = {},
                .changedStates = subscribedStateChanges(subscription, changed),
                .notifyConsumer = true,
            };
            for (const auto& [accountId, states] : event.changedStates)
            {
                static_cast<void>(accountId);
                for (const auto& [type, state] : states)
                {
                    static_cast<void>(state);
                    if (std::ranges::find(event.changedTypes, type) == event.changedTypes.end())
                        event.changedTypes.push_back(type);
                }
            }
            return event;
        }

        template <typename T>
        [[nodiscard]] std::variant<T, PushProtocolError> parseJson(const std::string_view json)
        {
            T value;
            std::string buffer{json};
            if (const auto error =
                    glz::read<glz::opts{.error_on_unknown_keys = false}>(value, buffer))
                return PushProtocolError{.message = std::string{glz::format_error(error, buffer)}};
            return value;
        }
    } // namespace

    PushPingIntervalResult parsePushPingInterval(const std::string_view eventData)
    {
        const auto parsed = parseJson<RawPushPing>(eventData);
        if (const auto* error = std::get_if<PushProtocolError>(&parsed))
            return error->message;

        const auto& ping = std::get<RawPushPing>(parsed);
        if (!ping.interval.has_value())
            return std::optional<std::chrono::seconds>{std::nullopt};

        unsigned int interval = 0;
        if (const auto* numeric = std::get_if<unsigned int>(&*ping.interval))
        {
            interval = *numeric;
        }
        else
        {
            const auto& text = std::get<std::string>(*ping.interval);
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), interval);
            if (error != std::errc{} || end != text.data() + text.size())
                return std::string{"Ping interval string is not an unsigned decimal integer."};
        }

        if (interval > 1000 && interval % 1000 == 0)
            interval /= 1000;
        return interval > 0 ? std::optional{std::chrono::seconds{interval}}
                            : std::optional<std::chrono::seconds>{std::nullopt};
    }

    PushMessage parseEventSourcePushMessage(const StateChangeSubscription& subscription,
                                            const std::string_view fallbackState,
                                            const std::string_view eventName,
                                            const std::string_view eventId,
                                            const std::string_view eventData)
    {
        if (eventName.empty() || eventName == "message")
            return PushMessageIgnored{};

        if (eventName == "ping")
        {
            const auto parsed = parsePushPingInterval(eventData);
            if (const auto* error = std::get_if<std::string>(&parsed))
                return PushProtocolError{.message = *error};
            const auto interval = std::get<std::optional<std::chrono::seconds>>(parsed);
            if (!interval.has_value())
                return PushProtocolError{.message = "JMAP ping event has no positive interval."};
            return PushPing{.interval = *interval};
        }

        if (eventName != "state")
            return PushMessageIgnored{};

        const auto parsed = parseJson<RawEventSourceStateChange>(eventData);
        if (const auto* error = std::get_if<PushProtocolError>(&parsed))
            return *error;
        auto stateChange = std::get<RawEventSourceStateChange>(parsed);

        if (stateChange.type == std::optional<std::string>{"connect"})
        {
            return StateChangeEvent{
                .newState = eventId.empty() ? std::string{fallbackState} : std::string{eventId},
                .changedTypes = {},
                .changedStates = {},
                .notifyConsumer = false,
            };
        }
        if (stateChange.objectType != std::optional<std::string>{"StateChange"})
            return PushProtocolError{.message = "Expected a JMAP StateChange event payload."};

        auto event = makeStateChangeEvent(
            subscription, fallbackState, std::move(stateChange.changed),
            eventId.empty() ? std::nullopt : std::optional{std::string{eventId}});
        return event.changedStates.empty() ? PushMessage{PushMessageIgnored{}}
                                           : PushMessage{std::move(event)};
    }

    PushMessage parseWebSocketPushMessage(const StateChangeSubscription& subscription,
                                          const std::string_view fallbackState,
                                          const std::string_view message)
    {
        const auto parsed = parseJson<RawWebSocketStateChange>(message);
        if (const auto* error = std::get_if<PushProtocolError>(&parsed))
            return *error;
        auto stateChange = std::get<RawWebSocketStateChange>(parsed);
        if (stateChange.type != std::optional<std::string>{"StateChange"})
            return PushMessageIgnored{};

        auto event = makeStateChangeEvent(subscription, fallbackState,
                                          std::move(stateChange.changed), stateChange.pushState);
        return event.changedStates.empty() ? PushMessage{PushMessageIgnored{}}
                                           : PushMessage{std::move(event)};
    }
} // namespace javelin::jmap::sync
