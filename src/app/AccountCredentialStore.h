#pragma once

#include <QHash>
#include <QString>

#include <memory>
#include <optional>
#include <variant>

namespace javelin::app
{
    struct AccountCredentialSecrets
    {
        QString accessToken;
        QString refreshToken;
        QString registrationAccessToken;

        [[nodiscard]] bool hasAccessToken() const
        {
            return !accessToken.isEmpty();
        }

        [[nodiscard]] bool empty() const
        {
            return accessToken.isEmpty() && refreshToken.isEmpty() &&
                   registrationAccessToken.isEmpty();
        }

        friend bool operator==(const AccountCredentialSecrets&,
                               const AccountCredentialSecrets&) = default;
    };

    struct AccountCredentialStoreError
    {
        QString detail;
    };

    using AccountCredentialLoadResult =
        std::variant<std::optional<AccountCredentialSecrets>, AccountCredentialStoreError>;

    class AccountCredentialStore
    {
      public:
        virtual ~AccountCredentialStore() = default;

        [[nodiscard]] virtual AccountCredentialLoadResult load(const QString& connectionId) = 0;
        [[nodiscard]] virtual std::optional<AccountCredentialStoreError>
        store(const QString& connectionId, const AccountCredentialSecrets& credentials) = 0;
        [[nodiscard]] virtual std::optional<AccountCredentialStoreError>
        remove(const QString& connectionId) = 0;
    };

    class MemoryAccountCredentialStore final : public AccountCredentialStore
    {
      public:
        [[nodiscard]] AccountCredentialLoadResult load(const QString& connectionId) override;
        [[nodiscard]] std::optional<AccountCredentialStoreError>
        store(const QString& connectionId, const AccountCredentialSecrets& credentials) override;
        [[nodiscard]] std::optional<AccountCredentialStoreError>
        remove(const QString& connectionId) override;

      private:
        QHash<QString, AccountCredentialSecrets> m_credentials;
    };

    [[nodiscard]] std::unique_ptr<AccountCredentialStore> makeKWalletAccountCredentialStore();
} // namespace javelin::app
