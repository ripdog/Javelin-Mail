#pragma once

#include "app/IdentityApplicationPorts.h"

namespace javelin::jmap::cache
{
    class AccountReader;
}
namespace javelin::jmap::identity
{
    class IdentityService;
}

namespace javelin::app
{
    class AccountConnectionProvider;
    class ApplicationErrorCoordinator;
    class MailCacheChangePublisher;
    class WorkScheduler;

    class IdentityCommandService final : public IdentityCommandPort
    {
      public:
        IdentityCommandService(javelin::jmap::identity::IdentityService& service,
                               javelin::jmap::cache::AccountReader& accountReader,
                               AccountConnectionProvider& connectionProvider,
                               ApplicationErrorCoordinator& errorCoordinator,
                               WorkScheduler& workScheduler,
                               MailCacheChangePublisher& cacheChangePublisher);

        [[nodiscard]] QCoro::Task<javelin::jmap::identity::IdentityListResult>
        requestSenderIdentities(std::string accountId) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::identity::IdentitySaveResult>
        saveSenderIdentity(std::string accountId,
                           javelin::jmap::domain::Identity identity) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::identity::IdentityDeleteResult>
        deleteSenderIdentity(std::string accountId, std::string identityId) override;

      private:
        javelin::jmap::identity::IdentityService& m_service;
        javelin::jmap::cache::AccountReader& m_accountReader;
        AccountConnectionProvider& m_connectionProvider;
        ApplicationErrorCoordinator& m_errorCoordinator;
        WorkScheduler& m_workScheduler;
        MailCacheChangePublisher& m_cacheChangePublisher;
    };
} // namespace javelin::app
