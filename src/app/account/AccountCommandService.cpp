#include "app/AccountCommandService.h"

#include "jmap/cache/AccountRepository.h"

namespace javelin::app
{
    AccountCommandService::AccountCommandService(
        javelin::jmap::cache::AccountRepository& repository)
        : m_repository(repository)
    {
    }

    QCoro::Task<std::optional<javelin::jmap::cache::DatabaseError>>
    AccountCommandService::removeConfiguredAccount(QString loginEmail, QString sessionUrl,
                                                   QStringList knownAccountIds)
    {
        co_return m_repository.removeConfiguredAccount(loginEmail, sessionUrl, knownAccountIds);
    }
} // namespace javelin::app
