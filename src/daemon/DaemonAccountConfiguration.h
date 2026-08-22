#pragma once

#include "app/AccountRuntimeManager.h"
#include "storage/DatabaseError.h"

#include <variant>
#include <vector>

namespace javelin::protocol
{
    struct SettingsSnapshot;
}

namespace javelin::jmap::cache
{
    class AccountRepository;
}

namespace javelin::app
{
    class AccountCredentialStore;

    using AccountSyncConfigurationsResult =
        std::variant<std::vector<AccountSyncConfiguration>, javelin::jmap::cache::DatabaseError>;

    [[nodiscard]] AccountSyncConfigurationsResult
    buildAccountSyncConfigurations(const javelin::protocol::SettingsSnapshot& snapshot,
                                   AccountCredentialStore& credentialStore,
                                   javelin::jmap::cache::AccountRepository& accountRepository);
} // namespace javelin::app
