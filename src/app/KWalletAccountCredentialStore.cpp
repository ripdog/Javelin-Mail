#include "app/AccountCredentialStore.h"

#include <KWallet>

#include <QMap>

#include <memory>
#include <utility>

namespace javelin::app
{
    namespace
    {
        class KWalletAccountCredentialStore final : public AccountCredentialStore
        {
          public:
            [[nodiscard]] AccountCredentialLoadResult load(const QString& connectionId) override
            {
                if (connectionId.isEmpty())
                {
                    return AccountCredentialStoreError{
                        .detail = QStringLiteral("Credential connection ID is empty.")};
                }
                if (const auto error = ensureWallet())
                    return *error;
                if (!m_wallet->hasEntry(connectionId))
                    return std::optional<AccountCredentialSecrets>{};

                QMap<QString, QString> values;
                if (m_wallet->readMap(connectionId, values) != 0)
                {
                    return AccountCredentialStoreError{
                        .detail = QStringLiteral("KWallet could not read account credentials.")};
                }
                const AccountCredentialSecrets credentials{
                    .accessToken = values.value(QStringLiteral("accessToken")),
                    .refreshToken = values.value(QStringLiteral("refreshToken")),
                    .registrationAccessToken =
                        values.value(QStringLiteral("registrationAccessToken")),
                };
                return std::optional<AccountCredentialSecrets>{credentials};
            }

            [[nodiscard]] std::optional<AccountCredentialStoreError>
            store(const QString& connectionId, const AccountCredentialSecrets& credentials) override
            {
                if (connectionId.isEmpty())
                {
                    return AccountCredentialStoreError{
                        .detail = QStringLiteral("Credential connection ID is empty.")};
                }
                if (credentials.empty())
                    return remove(connectionId);
                if (const auto error = ensureWallet())
                    return error;

                const QMap<QString, QString> values{
                    {QStringLiteral("accessToken"), credentials.accessToken},
                    {QStringLiteral("refreshToken"), credentials.refreshToken},
                    {QStringLiteral("registrationAccessToken"),
                     credentials.registrationAccessToken},
                };
                if (m_wallet->writeMap(connectionId, values) != 0 || m_wallet->sync() != 0)
                {
                    return AccountCredentialStoreError{
                        .detail = QStringLiteral("KWallet could not store account credentials.")};
                }
                return std::nullopt;
            }

            [[nodiscard]] std::optional<AccountCredentialStoreError>
            remove(const QString& connectionId) override
            {
                if (connectionId.isEmpty())
                    return std::nullopt;
                if (const auto error = ensureWallet())
                    return error;
                if (!m_wallet->hasEntry(connectionId))
                    return std::nullopt;
                if (m_wallet->removeEntry(connectionId) != 0 || m_wallet->sync() != 0)
                {
                    return AccountCredentialStoreError{
                        .detail = QStringLiteral("KWallet could not remove account credentials.")};
                }
                return std::nullopt;
            }

          private:
            [[nodiscard]] std::optional<AccountCredentialStoreError> ensureWallet()
            {
                if (m_wallet != nullptr && m_wallet->isOpen() &&
                    m_wallet->currentFolder() == QStringLiteral("Javelin Mail"))
                {
                    return std::nullopt;
                }

                m_wallet.reset(KWallet::Wallet::openWallet(KWallet::Wallet::NetworkWallet(), 0,
                                                           KWallet::Wallet::Synchronous));
                if (m_wallet == nullptr || !m_wallet->isOpen())
                {
                    return AccountCredentialStoreError{
                        .detail = QStringLiteral("KWallet is unavailable or remained locked.")};
                }
                const auto folder = QStringLiteral("Javelin Mail");
                if (!m_wallet->hasFolder(folder) && !m_wallet->createFolder(folder))
                {
                    return AccountCredentialStoreError{
                        .detail =
                            QStringLiteral("KWallet could not create the Javelin Mail folder.")};
                }
                if (!m_wallet->setFolder(folder))
                {
                    return AccountCredentialStoreError{
                        .detail =
                            QStringLiteral("KWallet could not open the Javelin Mail folder.")};
                }
                return std::nullopt;
            }

            std::unique_ptr<KWallet::Wallet> m_wallet;
        };
    } // namespace

    std::unique_ptr<AccountCredentialStore> makeKWalletAccountCredentialStore()
    {
        return std::make_unique<KWalletAccountCredentialStore>();
    }
} // namespace javelin::app
