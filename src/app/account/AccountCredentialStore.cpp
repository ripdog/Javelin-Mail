#include "app/AccountCredentialStore.h"

namespace javelin::app
{
    AccountCredentialLoadResult MemoryAccountCredentialStore::load(const QString& connectionId)
    {
        const auto found = m_credentials.constFind(connectionId);
        if (found == m_credentials.cend())
            return std::optional<AccountCredentialSecrets>{};
        return std::optional<AccountCredentialSecrets>{*found};
    }

    std::optional<AccountCredentialStoreError>
    MemoryAccountCredentialStore::store(const QString& connectionId,
                                        const AccountCredentialSecrets& credentials)
    {
        if (connectionId.isEmpty())
            return AccountCredentialStoreError{
                .detail = QStringLiteral("Credential connection ID is empty.")};
        if (credentials.empty())
            return remove(connectionId);
        m_credentials.insert(connectionId, credentials);
        return std::nullopt;
    }

    std::optional<AccountCredentialStoreError>
    MemoryAccountCredentialStore::remove(const QString& connectionId)
    {
        m_credentials.remove(connectionId);
        return std::nullopt;
    }
} // namespace javelin::app
