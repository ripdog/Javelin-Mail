#pragma once

#include "storage/DatabaseError.h"

#include "app/AccountApplicationPorts.h"

namespace javelin::jmap::cache
{
    class AccountRepository;
}

namespace javelin::app
{

    class AccountCommandService final : public AccountCommandPort
    {
      public:
        explicit AccountCommandService(javelin::jmap::cache::AccountRepository& repository);

        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        removeConfiguredAccount(const QString& loginEmail, const QString& sessionUrl,
                                const QStringList& knownAccountIds) override;

      private:
        javelin::jmap::cache::AccountRepository& m_repository;
    };

} // namespace javelin::app
