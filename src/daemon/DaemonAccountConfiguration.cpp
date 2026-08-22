#include "daemon/DaemonAccountConfiguration.h"

#include "app/AccountCredentialStore.h"
#include "jmap/cache/AccountRepository.h"
#include "protocol/SettingsContract.h"

#include <QStringList>

#include <algorithm>
#include <ranges>

namespace javelin::app
{
    namespace
    {
        [[nodiscard]] AccountConnectionSettings
        connectionSettings(const javelin::protocol::AccountSettings& settings,
                           const AccountCredentialSecrets& credentials)
        {
            return {.connectionId = settings.id.toStdString(),
                    .revision = settings.revision,
                    .displayName = settings.displayName.toStdString(),
                    .sessionUrl = settings.sessionUrl.toStdString(),
                    .loginEmail = settings.loginEmail.toStdString(),
                    .apiKey = credentials.accessToken.toStdString(),
                    .refreshToken = credentials.refreshToken.toStdString(),
                    .tokenEndpoint = settings.tokenEndpoint.toStdString(),
                    .oauthClientId = settings.oauthClientId.toStdString(),
                    .oauthIssuer = settings.oauthIssuer.toStdString(),
                    .oauthResource = settings.oauthResource.toStdString(),
                    .oauthScope = settings.oauthScope.toStdString(),
                    .revocationEndpoint = settings.revocationEndpoint.toStdString(),
                    .registrationClientUri = settings.registrationClientUri.toStdString(),
                    .registrationAccessToken = credentials.registrationAccessToken.toStdString()};
        }

        [[nodiscard]] const javelin::protocol::MailboxSelectionSettings*
        findSelection(const std::vector<javelin::protocol::MailboxSelectionSettings>& selections,
                      const QString& accountId)
        {
            const auto found = std::ranges::find(
                selections, accountId, &javelin::protocol::MailboxSelectionSettings::accountId);
            return found == selections.end() ? nullptr : &*found;
        }

        [[nodiscard]] std::vector<std::string> stringIds(const std::vector<QString>& ids)
        {
            std::vector<std::string> result;
            result.reserve(ids.size());
            for (const auto& id : ids)
                result.push_back(id.toStdString());
            return result;
        }

        [[nodiscard]] bool
        shouldConfigureMailAccount(javelin::jmap::cache::AccountReader& accountReader,
                                   const QString& accountId)
        {
            const auto cached = accountReader.findById(accountId.toStdString());
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&cached))
            {
                qWarning().noquote() << QStringLiteral("Could not inspect JMAP account capability")
                                     << accountId << error->message;
                return true;
            }
            const auto& account =
                std::get<std::optional<javelin::jmap::cache::CachedAccount>>(cached);
            return !account.has_value() || account->hasMailCapability;
        }
    } // namespace

    AccountSyncConfigurationsResult
    buildAccountSyncConfigurations(const javelin::protocol::SettingsSnapshot& snapshot,
                                   AccountCredentialStore& credentialStore,
                                   javelin::jmap::cache::AccountRepository& accountRepository)
    {
        std::vector<AccountSyncConfiguration> result;
        for (const auto& account : snapshot.accounts)
        {
            QStringList knownAccountIds;
            knownAccountIds.reserve(static_cast<qsizetype>(account.cachedAccountIds.size()));
            for (const auto& accountId : account.cachedAccountIds)
                knownAccountIds.push_back(accountId);
            if (const auto error = accountRepository.claimLegacyConnection(account.id.toStdString(),
                                                                           knownAccountIds))
            {
                return *error;
            }

            if (account.loginEmail.isEmpty() || !account.hasCredentials)
                continue;
            const auto loaded = credentialStore.load(account.id);
            const auto* stored = std::get_if<std::optional<AccountCredentialSecrets>>(&loaded);
            if (stored == nullptr || !stored->has_value() || !(*stored)->hasAccessToken())
                continue;
            const auto& credentials = **stored;

            const auto appendConfiguration = [&](const QString& accountId)
            {
                const auto* synced = findSelection(snapshot.syncedMailboxSelections, accountId);
                const auto* notifications =
                    findSelection(snapshot.notificationMailboxSelections, accountId);
                std::vector<QString> mailboxIds;
                if (synced != nullptr)
                    mailboxIds = synced->mailboxIds;
                if (notifications != nullptr)
                {
                    mailboxIds.insert(mailboxIds.end(), notifications->mailboxIds.begin(),
                                      notifications->mailboxIds.end());
                }
                std::ranges::sort(mailboxIds);
                mailboxIds.erase(std::ranges::unique(mailboxIds).begin(), mailboxIds.end());

                result.push_back({
                    .settings = connectionSettings(account, credentials),
                    .accountId = accountId.toStdString(),
                    .mailboxIds = stringIds(mailboxIds),
                    .fullSyncMailboxIds = synced == nullptr ? std::vector<std::string>{}
                                                            : stringIds(synced->mailboxIds),
                    .notificationMailboxIds = notifications == nullptr
                                                  ? std::vector<std::string>{}
                                                  : stringIds(notifications->mailboxIds),
                });
            };
            for (const auto& accountId : account.cachedAccountIds)
            {
                if (shouldConfigureMailAccount(accountRepository, accountId))
                    appendConfiguration(accountId);
            }
            if (account.cachedAccountIds.empty())
                appendConfiguration(account.id);
        }
        return result;
    }
} // namespace javelin::app
