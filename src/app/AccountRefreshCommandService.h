#pragma once

#include "app/AccountRefreshApplicationPorts.h"

namespace javelin::app
{
    class AccountRuntimeManager;
    class ContactApplicationService;

    class AccountRefreshCommandService final : public AccountRefreshPort
    {
      public:
        AccountRefreshCommandService(AccountRuntimeManager& accountRuntime,
                                     ContactApplicationService& contacts);

        [[nodiscard]] bool requestAccountSynchronization(std::string_view accountId) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::LiveRefreshResult>
        bootstrapAccount(AccountBootstrapIntent intent) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactRefreshResult>
        requestContacts(std::string ownerAccountId) override;

      private:
        AccountRuntimeManager& m_accountRuntime;
        ContactApplicationService& m_contacts;
    };
} // namespace javelin::app
