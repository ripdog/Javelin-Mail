#pragma once

#include "jmap/sync/StateChangeSource.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace javelin::jmap::sync
{
    inline constexpr auto requestedPushPingInterval = std::chrono::seconds{30};
    inline constexpr auto maximumPushActivityTimeout = std::chrono::seconds{350};

    [[nodiscard]] constexpr std::chrono::seconds
    pushActivityTimeout(const std::chrono::seconds serverPingInterval)
    {
        constexpr auto grace = std::chrono::seconds{15};
        constexpr auto maximumAcceptedInterval = std::chrono::seconds{300};
        return std::min(std::min(serverPingInterval, maximumAcceptedInterval) * 2 + grace,
                        maximumPushActivityTimeout);
    }

    struct PushMessageIgnored
    {
    };

    struct PushPing
    {
        std::chrono::seconds interval;
    };

    struct PushProtocolError
    {
        std::string message;
    };

    using PushMessage = std::variant<PushMessageIgnored, PushPing, StateChangeEvent,
                                     CalendarAlertEvent, PushProtocolError>;
    using PushPingIntervalResult = std::variant<std::optional<std::chrono::seconds>, std::string>;

    [[nodiscard]] PushPingIntervalResult parsePushPingInterval(std::string_view eventData);

    [[nodiscard]] PushMessage
    parseEventSourcePushMessage(const StateChangeSubscription& subscription,
                                std::string_view fallbackState, std::string_view eventName,
                                std::string_view eventId, std::string_view eventData);

    [[nodiscard]] PushMessage parseWebSocketPushMessage(const StateChangeSubscription& subscription,
                                                        std::string_view fallbackState,
                                                        std::string_view message);
} // namespace javelin::jmap::sync
