#include "app/AccountRefreshCommandService.h"

#include "app/MailApplicationService.h"

#include <utility>

namespace javelin::app
{
    AccountRefreshCommandService::AccountRefreshCommandService(MailApplicationService& service)
        : m_service(service)
    {
    }

    bool
    AccountRefreshCommandService::requestAccountSynchronization(const std::string_view accountId)
    {
        return m_service.requestAccountSynchronization(accountId);
    }

    QCoro::Task<javelin::jmap::LiveRefreshResult>
    AccountRefreshCommandService::bootstrapAccount(AccountBootstrapIntent intent)
    {
        return m_service.bootstrapAccount(std::move(intent));
    }

    QCoro::Task<javelin::jmap::contacts::ContactRefreshResult>
    AccountRefreshCommandService::requestContacts(std::string ownerAccountId)
    {
        return m_service.requestContacts(std::move(ownerAccountId));
    }
} // namespace javelin::app
