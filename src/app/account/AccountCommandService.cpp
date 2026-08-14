#include "app/AccountCommandService.h"

#include "jmap/cache/AccountRepository.h"

namespace javelin::app
{
    AccountCommandService::AccountCommandService(
        javelin::jmap::cache::AccountRepository& repository)
        : m_repository(repository)
    {
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    AccountCommandService::removeConfiguredAccount(const QString& loginEmail,
                                                   const QString& sessionUrl,
                                                   const QStringList& knownAccountIds)
    {
        return m_repository.removeConfiguredAccount(loginEmail, sessionUrl, knownAccountIds);
    }
} // namespace javelin::app
