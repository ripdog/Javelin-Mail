#pragma once

#include "protocol/ActionContract.h"

#include <QStringView>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>

namespace javelin::protocol::actions
{
    enum class ActionDomain : std::uint8_t
    {
        Account,
        Calendar,
        Compose,
        Contact,
        Mail,
        Sieve,
        Identity,
        History,
        Work,
        Developer,
    };

    enum class AdmissionSemantics : std::uint8_t
    {
        Immediate,
        Asynchronous,
    };

    enum class ReplayPolicy : std::uint8_t
    {
        Never,
        Reexecute,
    };

    using ChangedDomainMask = std::uint16_t;

    [[nodiscard]] constexpr ChangedDomainMask domainBit(const ChangedDomain domain)
    {
        return static_cast<ChangedDomainMask>(1U << static_cast<unsigned int>(domain));
    }

    template <typename... Domains>
    [[nodiscard]] consteval ChangedDomainMask changedDomains(const Domains... domains)
    {
        return (ChangedDomainMask{} | ... | domainBit(domains));
    }

    [[nodiscard]] inline std::vector<ChangedDomain>
    expandChangedDomains(const ChangedDomainMask mask)
    {
        std::vector<ChangedDomain> result;
        for (std::uint8_t raw = 0;
             raw <= static_cast<std::uint8_t>(ChangedDomain::UserVisibleFailures); ++raw)
        {
            const auto domain = static_cast<ChangedDomain>(raw);
            if ((mask & domainBit(domain)) != 0)
                result.push_back(domain);
        }
        return result;
    }

    template <std::uint16_t Id, ActionDomain Domain, AdmissionSemantics Admission,
              ReplayPolicy Replay, ChangedDomainMask ChangedDomains, typename RequestType,
              typename ResultType, std::size_t MaximumPayloadBytes = 1024 * 1024>
    struct Descriptor
    {
        static constexpr ActionId id{Id};
        static constexpr ActionDomain domain = Domain;
        static constexpr AdmissionSemantics admission = Admission;
        static constexpr ReplayPolicy replay = Replay;
        static constexpr ChangedDomainMask changedDomains = ChangedDomains;
        static constexpr std::uint16_t requestSchemaVersion = 1;
        static constexpr std::uint16_t resultSchemaVersion = 1;
        static constexpr std::size_t maximumPayloadBytes = MaximumPayloadBytes;
        using Request = RequestType;
        using Result = ResultType;
    };

    template <std::size_t Size> struct FixedActionName
    {
        char value[Size]{};

        consteval FixedActionName(const char (&source)[Size])
        {
            for (std::size_t index = 0; index < Size; ++index)
                value[index] = source[index];
        }

        [[nodiscard]] constexpr std::string_view view() const
        {
            return {value, Size - 1};
        }
    };

    template <std::size_t Size> FixedActionName(const char (&)[Size]) -> FixedActionName<Size>;

    template <typename Action, FixedActionName Name> struct RegisteredAction : Action
    {
        static constexpr auto registeredName = Name;
    };

    struct ActionMetadata
    {
        ActionId id;
        ActionDomain domain;
        AdmissionSemantics admission;
        ReplayPolicy replay;
        ChangedDomainMask changedDomains;
        std::string_view name;
        std::uint16_t requestSchemaVersion;
        std::uint16_t resultSchemaVersion;
        std::size_t maximumPayloadBytes;
    };

    template <typename Action> [[nodiscard]] constexpr ActionMetadata metadata()
    {
        return {.id = Action::id,
                .domain = Action::domain,
                .admission = Action::admission,
                .replay = Action::replay,
                .changedDomains = Action::changedDomains,
                .name = Action::registeredName.view(),
                .requestSchemaVersion = Action::requestSchemaVersion,
                .resultSchemaVersion = Action::resultSchemaVersion,
                .maximumPayloadBytes = Action::maximumPayloadBytes};
    }
} // namespace javelin::protocol::actions
