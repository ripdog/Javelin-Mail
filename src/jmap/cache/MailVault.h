#pragma once

#include "jmap/cache/Database.h"

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <memory>
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

    class MailVaultLease final
    {
      public:
        MailVaultLease() = default;
        MailVaultLease(const MailVaultLease&) = delete;
        MailVaultLease& operator=(const MailVaultLease&) = delete;
        MailVaultLease(MailVaultLease&&) noexcept = default;
        MailVaultLease& operator=(MailVaultLease&&) noexcept = default;
        ~MailVaultLease();

        [[nodiscard]] bool isValid() const;
        [[nodiscard]] const MailVaultObject& object() const;
        [[nodiscard]] std::variant<QByteArray, MailVaultError> read() const;

      private:
        struct State;
        explicit MailVaultLease(std::shared_ptr<State> state);

        std::shared_ptr<State> m_state;

        friend class MailVault;
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
        [[nodiscard]] std::variant<MailVaultObject, MailVaultError>
        stage(const QByteArray& payload) const;
        [[nodiscard]] std::variant<MailVaultLease, MailVaultError>
        acquireLease(const MailVaultObject& object) const;
        [[nodiscard]] bool isLeased(const MailVaultObject& object) const;
        [[nodiscard]] std::variant<QByteArray, MailVaultError>
        read(const MailVaultObject& object) const;
        [[nodiscard]] std::optional<MailVaultError> evict(const MailVaultObject& object) const;
        static void releaseAllLeases();
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
        [[nodiscard]] std::variant<MailVaultObject, MailVaultError>
        installAt(const QByteArray& payload, QString relativePrefix) const;

        QString m_rootPath;
    };

} // namespace javelin::jmap::cache
