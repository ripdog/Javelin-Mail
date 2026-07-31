#include "jmap/cache/MailVault.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrl>

#include <filesystem>
#include <system_error>

namespace javelin::jmap::cache
{
    namespace
    {
        [[nodiscard]] QString component(const std::string_view value)
        {
            const auto encoded = QUrl::toPercentEncoding(
                QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())));
            return encoded.isEmpty() ? QStringLiteral("_") : QString::fromLatin1(encoded);
        }

        [[nodiscard]] MailVaultError error(const QString& operation, const QString& detail)
        {
            return {.message = operation + QStringLiteral(": ") + detail};
        }

        [[nodiscard]] std::optional<MailVaultError> ensureDirectory(const QString& path)
        {
            if (QDir{}.mkpath(path))
            {
                QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                QFileDevice::ExeOwner);
                return std::nullopt;
            }
            return error(QStringLiteral("Create mail vault directory"), path);
        }

        [[nodiscard]] std::optional<MailVaultError> writeJson(const QString& path,
                                                              const QJsonObject& object)
        {
            QSaveFile file{path};
            file.setDirectWriteFallback(false);
            if (!file.open(QIODevice::WriteOnly))
            {
                return error(QStringLiteral("Open mail vault metadata"), file.errorString());
            }
            file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            if (file.write(QJsonDocument{object}.toJson(QJsonDocument::Indented)) < 0)
            {
                return error(QStringLiteral("Write mail vault metadata"), file.errorString());
            }
            if (!file.commit())
            {
                return error(QStringLiteral("Commit mail vault metadata"), file.errorString());
            }
            return std::nullopt;
        }
    } // namespace

    MailVault::MailVault(QString rootPath) : m_rootPath(QDir::cleanPath(std::move(rootPath)))
    {
    }

    MailVault MailVault::forDatabase(const DatabaseConnection& connection)
    {
        const QFileInfo databaseInfo{connection.database().databaseName()};
        return MailVault{
            QDir(databaseInfo.absolutePath()).filePath(QStringLiteral("mail-vault/v1"))};
    }

    MailVault MailVault::forDatabase(const DatabaseReadView& connection)
    {
        const QFileInfo databaseInfo{connection.database().databaseName()};
        return MailVault{
            QDir(databaseInfo.absolutePath()).filePath(QStringLiteral("mail-vault/v1"))};
    }

    const QString& MailVault::rootPath() const
    {
        return m_rootPath;
    }

    QString MailVault::searchIndexPath(const std::string_view accountId) const
    {
        return QDir(m_rootPath)
            .filePath(QStringLiteral("indexes/%1/search.sqlite3").arg(component(accountId)));
    }

    std::variant<MailVaultObject, MailVaultError>
    MailVault::install(const QByteArray& payload) const
    {
        const QString hash = QString::fromLatin1(
            QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
        const QString relativePath = QStringLiteral("objects/sha256/%1/%2/%3.eml")
                                         .arg(hash.first(2), hash.sliced(2, 2), hash);
        const QString absolutePath = QDir(m_rootPath).filePath(relativePath);
        const QFileInfo existing{absolutePath};
        if (existing.exists())
        {
            if (existing.size() != payload.size())
            {
                return error(QStringLiteral("Verify existing mail vault object"), absolutePath);
            }
            return MailVaultObject{.contentHash = hash.toStdString(),
                                   .relativePath = relativePath,
                                   .size = static_cast<std::uint64_t>(payload.size())};
        }

        if (const auto directoryError = ensureDirectory(existing.absolutePath()))
        {
            return *directoryError;
        }
        QSaveFile file{absolutePath};
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly))
        {
            return error(QStringLiteral("Open mail vault object"), file.errorString());
        }
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        if (file.write(payload) != payload.size())
        {
            return error(QStringLiteral("Write mail vault object"), file.errorString());
        }
        if (!file.commit())
        {
            return error(QStringLiteral("Commit mail vault object"), file.errorString());
        }
        return MailVaultObject{.contentHash = hash.toStdString(),
                               .relativePath = relativePath,
                               .size = static_cast<std::uint64_t>(payload.size())};
    }

    std::variant<QByteArray, MailVaultError> MailVault::read(const MailVaultObject& object) const
    {
        QFile file{objectPath(object)};
        if (!file.open(QIODevice::ReadOnly))
        {
            return error(QStringLiteral("Open mail vault object"), file.errorString());
        }
        const auto payload = file.readAll();
        if (file.error() != QFileDevice::NoError)
        {
            return error(QStringLiteral("Read mail vault object"), file.errorString());
        }
        const auto hash = QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();
        if (hash.toStdString() != object.contentHash)
        {
            return error(QStringLiteral("Verify mail vault object"), objectPath(object));
        }
        return payload;
    }

    std::optional<MailVaultError> MailVault::project(const std::string_view accountId,
                                                     const std::string_view mailboxId,
                                                     const std::string_view emailId,
                                                     const MailVaultObject& object) const
    {
        const QString directory =
            QDir(mailboxPath(accountId, mailboxId)).filePath(QStringLiteral("messages"));
        if (const auto directoryError = ensureDirectory(directory))
        {
            return directoryError;
        }
        const QString destination =
            QDir(directory).filePath(component(emailId) + QStringLiteral(".eml"));
        if (QFileInfo::exists(destination) && !QFile::remove(destination))
        {
            return error(QStringLiteral("Replace mailbox mail projection"), destination);
        }

        std::error_code linkError;
        std::filesystem::create_hard_link(objectPath(object).toStdString(),
                                          destination.toStdString(), linkError);
        if (linkError)
        {
            return error(QStringLiteral("Create mailbox mail hard link"),
                         QString::fromStdString(linkError.message()));
        }
        return std::nullopt;
    }

    std::optional<MailVaultError> MailVault::removeProjection(const std::string_view accountId,
                                                              const std::string_view mailboxId,
                                                              const std::string_view emailId) const
    {
        const QString path =
            QDir(mailboxPath(accountId, mailboxId))
                .filePath(QStringLiteral("messages/%1.eml").arg(component(emailId)));
        if (QFileInfo::exists(path) && !QFile::remove(path))
        {
            return error(QStringLiteral("Remove mailbox mail projection"), path);
        }
        return std::nullopt;
    }

    std::optional<MailVaultError>
    MailVault::writeAccountMetadata(const std::string_view accountId,
                                    const std::string_view emailAddress) const
    {
        const QString directory = accountPath(accountId);
        if (const auto directoryError = ensureDirectory(directory))
        {
            return directoryError;
        }
        return writeJson(QDir(directory).filePath(QStringLiteral("account.json")),
                         QJsonObject{{QStringLiteral("accountId"),
                                      QString::fromStdString(std::string{accountId})},
                                     {QStringLiteral("emailAddress"),
                                      QString::fromStdString(std::string{emailAddress})}});
    }

    std::optional<MailVaultError> MailVault::writeMailboxMetadata(const std::string_view accountId,
                                                                  const std::string_view mailboxId,
                                                                  const std::string_view name) const
    {
        const QString directory = mailboxPath(accountId, mailboxId);
        if (const auto directoryError = ensureDirectory(directory))
        {
            return directoryError;
        }
        return writeJson(
            QDir(directory).filePath(QStringLiteral("mailbox.json")),
            QJsonObject{
                {QStringLiteral("mailboxId"), QString::fromStdString(std::string{mailboxId})},
                {QStringLiteral("name"), QString::fromStdString(std::string{name})}});
    }

    QString MailVault::objectPath(const MailVaultObject& object) const
    {
        return QDir(m_rootPath).filePath(object.relativePath);
    }

    QString MailVault::accountPath(const std::string_view accountId) const
    {
        return QDir(m_rootPath).filePath(QStringLiteral("accounts/%1").arg(component(accountId)));
    }

    QString MailVault::mailboxPath(const std::string_view accountId,
                                   const std::string_view mailboxId) const
    {
        return QDir(accountPath(accountId))
            .filePath(QStringLiteral("mailboxes/%1").arg(component(mailboxId)));
    }

} // namespace javelin::jmap::cache
