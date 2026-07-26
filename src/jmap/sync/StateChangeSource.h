#pragma once

#include "jmap/api/Error.h"

#include <QCoroTask>

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace javelin::jmap::sync
{

    struct StateChangeSubscription
    {
        std::string accountId;
        std::string lastState;
        std::vector<std::string> types;
        std::vector<std::string> groupwareAccountIds;
    };

    using TypeStateMap = std::unordered_map<std::string, std::string>;
    using AccountTypeStateMap = std::unordered_map<std::string, TypeStateMap>;

    [[nodiscard]] inline bool isGroupwareStateType(const std::string_view type)
    {
        return type == "Calendar" || type == "CalendarEvent" || type == "AddressBook" ||
               type == "ContactCard";
    }

    [[nodiscard]] inline TypeStateMap
    subscribedStateChanges(const StateChangeSubscription& subscription,
                           const AccountTypeStateMap& changed)
    {
        TypeStateMap result;
        if (const auto primary = changed.find(subscription.accountId); primary != changed.end())
            result = primary->second;

        for (const auto& accountId : subscription.groupwareAccountIds)
        {
            if (accountId == subscription.accountId)
                continue;
            const auto account = changed.find(accountId);
            if (account == changed.end())
                continue;
            for (const auto& [type, state] : account->second)
                if (isGroupwareStateType(type))
                    result.insert_or_assign(type, state);
        }
        return result;
    }

    struct StateChangeEvent
    {
        std::string newState;
        std::vector<std::string> changedTypes;
        std::unordered_map<std::string, std::string> changedStates;
        bool notifyConsumer = true;
    };

    struct StateChangeStreamSummary
    {
        std::string lastState;
        std::size_t updateCount = 0;
    };

    class StateChangeCancellation
    {
      public:
        void cancel();
        [[nodiscard]] bool isCancelled() const;

      private:
        bool m_cancelled = false;
    };

    using StateChangeSourceResult =
        std::variant<StateChangeStreamSummary, javelin::jmap::api::TransportError>;

    enum class StateChangeConnectionStatus
    {
        Disconnected,
        Connecting,
        Connected,
    };

    using StateChangeStatusCallback = std::function<void(StateChangeConnectionStatus)>;

    class StateChangeConsumer
    {
      public:
        virtual ~StateChangeConsumer() = default;

        [[nodiscard]] virtual QCoro::Task<void> onStateChange(StateChangeEvent event) = 0;
    };

    class StateChangeSource
    {
      public:
        virtual ~StateChangeSource() = default;
        virtual void cancel()
        {
        }

        [[nodiscard]] virtual QCoro::Task<StateChangeSourceResult>
        consume(StateChangeSubscription subscription, StateChangeConsumer& consumer,
                StateChangeCancellation& cancellation) = 0;
    };

} // namespace javelin::jmap::sync
