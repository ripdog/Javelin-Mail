#pragma once

#include "app/AccountRefreshApplicationPorts.h"

namespace javelin::app
{
    class MailApplicationService;

    class AccountRefreshCommandService final : public AccountRefreshPort
    {
      public:
        explicit AccountRefreshCommandService(MailApplicationService& service);

        [[nodiscard]] bool requestAccountSynchronization(std::string_view accountId) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::LiveRefreshResult>
        bootstrapAccount(AccountBootstrapIntent intent) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactRefreshResult>
        requestContacts(std::string ownerAccountId) override;

      private:
        MailApplicationService& m_service;
    };
} // namespace javelin::app
