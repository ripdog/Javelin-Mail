#pragma once

#include "jmap/cache/Database.h"

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace javelin::jmap::cache
{

    struct MailVaultError
    {
        QString message;
    };

    struct MailVaultObject
    {
        std::string contentHash;
        QString relativePath;
        std::uint64_t size = 0;
    };

    class MailVault
    {
      public:
        explicit MailVault(QString rootPath);

        [[nodiscard]] static MailVault forDatabase(const DatabaseConnection& connection);
        [[nodiscard]] static MailVault forDatabase(const DatabaseReadView& connection);
        [[nodiscard]] const QString& rootPath() const;
        [[nodiscard]] QString searchIndexPath(std::string_view accountId) const;
        [[nodiscard]] std::variant<MailVaultObject, MailVaultError>
        install(const QByteArray& payload) const;
        [[nodiscard]] std::variant<QByteArray, MailVaultError>
        read(const MailVaultObject& object) const;
        [[nodiscard]] std::optional<MailVaultError> project(std::string_view accountId,
                                                            std::string_view mailboxId,
                                                            std::string_view emailId,
                                                            const MailVaultObject& object) const;
        [[nodiscard]] std::optional<MailVaultError>
        removeProjection(std::string_view accountId, std::string_view mailboxId,
                         std::string_view emailId) const;
        [[nodiscard]] std::optional<MailVaultError>
        writeAccountMetadata(std::string_view accountId, std::string_view emailAddress) const;
        [[nodiscard]] std::optional<MailVaultError>
        writeMailboxMetadata(std::string_view accountId, std::string_view mailboxId,
                             std::string_view name) const;

      private:
        [[nodiscard]] QString objectPath(const MailVaultObject& object) const;
        [[nodiscard]] QString accountPath(std::string_view accountId) const;
        [[nodiscard]] QString mailboxPath(std::string_view accountId,
                                          std::string_view mailboxId) const;

        QString m_rootPath;
    };

} // namespace javelin::jmap::cache
