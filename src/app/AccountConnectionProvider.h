#pragma once

#include "app/AccountConnectionSettings.h"

#include <optional>
#include <string_view>

namespace javelin::app
{
    class AccountConnectionProvider
    {
      public:
        virtual ~AccountConnectionProvider() = default;

        [[nodiscard]] virtual std::optional<AccountConnectionSettings>
        connectionSettingsFor(std::string_view ownerAccountId) const = 0;
    };
} // namespace javelin::app
