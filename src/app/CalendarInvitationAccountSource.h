#pragma once

#include "app/AccountConnectionProvider.h"

#include <string>
#include <vector>

namespace javelin::app
{
    class CalendarInvitationAccountSource : public AccountConnectionProvider
    {
      public:
        ~CalendarInvitationAccountSource() override = default;

        [[nodiscard]] virtual std::vector<std::string> configuredAccountIds() const = 0;
    };
} // namespace javelin::app
