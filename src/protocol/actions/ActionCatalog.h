#pragma once

#include "protocol/actions/AccountActions.h"
#include "protocol/actions/CalendarActions.h"
#include "protocol/actions/ComposeActions.h"
#include "protocol/actions/ContactActions.h"
#include "protocol/actions/DeveloperActions.h"
#include "protocol/actions/HistoryActions.h"
#include "protocol/actions/IdentityActions.h"
#include "protocol/actions/MailActions.h"
#include "protocol/actions/SieveActions.h"
#include "protocol/actions/WorkActions.h"

#include <QString>

#include <array>
#include <optional>
#include <tuple>
#include <utility>

namespace javelin::protocol::actions
{
    using AllActionTypes = decltype(std::tuple_cat(
        std::declval<AccountActionTypes>(), std::declval<CalendarActionTypes>(),
        std::declval<ComposeActionTypes>(), std::declval<ContactActionTypes>(),
        std::declval<MailActionTypes>(), std::declval<SieveActionTypes>(),
        std::declval<IdentityActionTypes>(), std::declval<HistoryActionTypes>(),
        std::declval<WorkActionTypes>(), std::declval<DeveloperActionTypes>()));

    template <typename Tuple, std::size_t... Index>
    [[nodiscard]] consteval auto makeActionCatalog(std::index_sequence<Index...>)
    {
        return std::array{metadata<std::tuple_element_t<Index, Tuple>>()...};
    }

    inline constexpr auto actionCatalog = makeActionCatalog<AllActionTypes>(
        std::make_index_sequence<std::tuple_size_v<AllActionTypes>>{});

    static_assert(
        []
        {
            std::array<bool, actionCatalog.size()> seen{};
            for (const auto& item : actionCatalog)
            {
                if (item.id.value >= actionCatalog.size() || seen[item.id.value] ||
                    item.name.empty())
                    return false;
                seen[item.id.value] = true;
            }
            for (const bool present : seen)
            {
                if (!present)
                    return false;
            }
            return true;
        }(),
        "Action ids are part of the wire contract and must remain unique and contiguous");

    [[nodiscard]] constexpr std::optional<ActionMetadata> findActionMetadata(const ActionId id)
    {
        for (const auto& item : actionCatalog)
        {
            if (item.id == id)
                return item;
        }
        return std::nullopt;
    }

    [[nodiscard]] constexpr bool isKnownAction(const ActionId id)
    {
        return findActionMetadata(id).has_value();
    }

    [[nodiscard]] inline QString actionName(const ActionId id)
    {
        if (const auto item = findActionMetadata(id))
            return QString::fromLatin1(item->name.data(),
                                       static_cast<qsizetype>(item->name.size()));
        return QStringLiteral("UnknownAction(%1)").arg(id.value);
    }
} // namespace javelin::protocol::actions
