#include "app/AccountRefreshCommandService.h"

#include "app/AccountRuntimeManager.h"
#include "app/ContactApplicationService.h"

#include <utility>

namespace javelin::app
{
    AccountRefreshCommandService::AccountRefreshCommandService(
        AccountRuntimeManager& accountRuntime, ContactApplicationService& contacts)
        : m_accountRuntime(accountRuntime), m_contacts(contacts)
    {
    }

    bool
    AccountRefreshCommandService::requestAccountSynchronization(const std::string_view accountId)
    {
        return m_accountRuntime.requestAccountSynchronization(accountId);
    }

    QCoro::Task<javelin::jmap::LiveRefreshResult>
    AccountRefreshCommandService::bootstrapAccount(AccountBootstrapIntent intent)
    {
        return m_accountRuntime.bootstrapAccount(std::move(intent));
    }

    QCoro::Task<javelin::jmap::contacts::ContactRefreshResult>
    AccountRefreshCommandService::requestContacts(std::string ownerAccountId)
    {
        return m_contacts.requestContacts(std::move(ownerAccountId));
    }
} // namespace javelin::app
