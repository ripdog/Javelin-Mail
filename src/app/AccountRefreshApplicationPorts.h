#pragma once

#include "app/AccountConnectionSettings.h"
#include "app/ContactApplicationPorts.h"
#include "jmap/AccountBootstrapClient.h"

#include <QCoroTask>

#include <string>
#include <string_view>
#include <vector>

namespace javelin::app
{
    struct AccountBootstrapIntent
    {
        AccountConnectionSettings settings;
        std::vector<std::string> mailboxIds;
    };

    class AccountRefreshPort : public ContactRefreshPort
    {
      public:
        ~AccountRefreshPort() override = default;

        [[nodiscard]] virtual bool requestAccountSynchronization(std::string_view accountId) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::LiveRefreshResult>
        bootstrapAccount(AccountBootstrapIntent intent) = 0;
    };
} // namespace javelin::app
