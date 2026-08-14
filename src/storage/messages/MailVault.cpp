#include "jmap/cache/MailVault.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QUrl>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace javelin::jmap::cache
{
    namespace
    {
        struct LeaseToken
        {
            std::string path;
            std::atomic_bool active = true;
        };

        class LeaseRegistry final
        {
          public:
            [[nodiscard]] bool acquire(const std::string& path,
                                       const std::shared_ptr<LeaseToken>& token,
                                       const std::function<bool()>& exists)
            {
                std::scoped_lock lock{m_mutex};
                if (!exists())
                    return false;
                ++m_counts[path];
                auto& tokens = m_tokens[path];
                std::erase_if(tokens, [](const auto& candidate) { return candidate.expired(); });
                tokens.push_back(token);
                return true;
            }

            void release(const std::shared_ptr<LeaseToken>& token)
            {
                if (!token)
                    return;
                std::scoped_lock lock{m_mutex};
                if (token->active.exchange(false))
                {
                    const auto found = m_counts.find(token->path);
                    if (found != m_counts.end())
                    {
                        if (found->second <= 1)
                            m_counts.erase(found);
                        else
                            --found->second;
                    }
                }
                const auto tokens = m_tokens.find(token->path);
                if (tokens == m_tokens.end())
                    return;
                std::erase_if(tokens->second,
                              [](const auto& candidate) { return candidate.expired(); });
                if (tokens->second.empty())
                    m_tokens.erase(tokens);
            }

            [[nodiscard]] std::optional<bool> evict(const std::string& path,
                                                    const std::function<bool()>& remove)
            {
                std::scoped_lock lock{m_mutex};
                const auto found = m_counts.find(path);
                if (found != m_counts.end() && found->second > 0)
                    return std::nullopt;
                return remove();
            }

            [[nodiscard]] bool isLeased(const std::string& path) const
            {
                std::scoped_lock lock{m_mutex};
                const auto found = m_counts.find(path);
                return found != m_counts.end() && found->second > 0;
            }

            void releaseAll()
            {
                std::scoped_lock lock{m_mutex};
                for (auto& [path, tokens] : m_tokens)
                {
                    Q_UNUSED(path);
                    for (const auto& token : tokens)
                    {
                        if (const auto lease = token.lock())
                            lease->active.store(false);
                    }
                }
                m_counts.clear();
                m_tokens.clear();
            }

          private:
            mutable std::mutex m_mutex;
            std::unordered_map<std::string, std::size_t> m_counts;
            std::unordered_map<std::string, std::vector<std::weak_ptr<LeaseToken>>> m_tokens;
        };

        [[nodiscard]] LeaseRegistry& leaseRegistry()
        {
            static LeaseRegistry registry;
            return registry;
        }

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

    struct MailVaultLease::State
    {
        MailVaultObject object;
        QString path;
        std::shared_ptr<LeaseToken> token;

        ~State()
        {
            leaseRegistry().release(token);
        }
    };

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

    std::variant<MailVaultObject, MailVaultError> MailVault::installAt(const QByteArray& payload,
                                                                       QString relativePrefix) const
    {
        const QString hash = QString::fromLatin1(
            QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
        const QString relativePath =
            QStringLiteral("%1/%2/%3/%4.eml")
                .arg(std::move(relativePrefix), hash.first(2), hash.sliced(2, 2), hash);
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

    std::variant<MailVaultObject, MailVaultError>
    MailVault::install(const QByteArray& payload) const
    {
        return installAt(payload, QStringLiteral("objects/sha256"));
    }

    std::variant<MailVaultObject, MailVaultError> MailVault::stage(const QByteArray& payload) const
    {
        return installAt(payload, QStringLiteral("staging/sha256"));
    }

    std::optional<MailVaultError> MailVault::cleanupIncoming() const
    {
        QDir directory{QDir(m_rootPath).filePath(QStringLiteral("incoming"))};
        if (!directory.exists())
            return std::nullopt;
        const auto entries = directory.entryInfoList({QStringLiteral("download-*.eml")},
                                                     QDir::Files | QDir::NoSymLinks);
        for (const auto& entry : entries)
        {
            if (!QFile::remove(entry.absoluteFilePath()))
                return error(QStringLiteral("Remove stale incoming mail vault object"),
                             entry.absoluteFilePath());
        }
        return std::nullopt;
    }

    std::variant<QString, MailVaultError> MailVault::prepareIncoming() const
    {
        const QString directory = QDir(m_rootPath).filePath(QStringLiteral("incoming"));
        if (const auto directoryError = ensureDirectory(directory))
            return *directoryError;

        QTemporaryFile file{QDir(directory).filePath(QStringLiteral("download-XXXXXX.eml"))};
        file.setAutoRemove(false);
        if (!file.open())
            return error(QStringLiteral("Create incoming mail vault object"), file.errorString());
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        const QString path = file.fileName();
        file.close();
        return path;
    }

    std::variant<MailVaultObject, MailVaultError>
    MailVault::installIncoming(QString incomingPath) const
    {
        QFile incoming{incomingPath};
        if (!incoming.open(QIODevice::ReadOnly))
            return error(QStringLiteral("Open incoming mail vault object"), incoming.errorString());

        QCryptographicHash hasher{QCryptographicHash::Sha256};
        QByteArray buffer(64 * 1024, Qt::Uninitialized);
        while (true)
        {
            const qint64 count = incoming.read(buffer.data(), buffer.size());
            if (count < 0)
                return error(QStringLiteral("Read incoming mail vault object"),
                             incoming.errorString());
            if (count == 0)
                break;
            hasher.addData(QByteArrayView{buffer.constData(), count});
        }
        const auto size = static_cast<std::uint64_t>(incoming.size());
        const QString hash = QString::fromLatin1(hasher.result().toHex());
        const QString relativePath = QStringLiteral("objects/sha256/%1/%2/%3.eml")
                                         .arg(hash.first(2), hash.sliced(2, 2), hash);
        const QString absolutePath = QDir(m_rootPath).filePath(relativePath);
        incoming.close();

        const QFileInfo existing{absolutePath};
        if (existing.exists())
        {
            if (static_cast<std::uint64_t>(existing.size()) != size)
                return error(QStringLiteral("Verify existing mail vault object"), absolutePath);
            if (!QFile::remove(incomingPath))
                return error(QStringLiteral("Remove duplicate incoming mail vault object"),
                             incomingPath);
            return MailVaultObject{
                .contentHash = hash.toStdString(), .relativePath = relativePath, .size = size};
        }

        if (const auto directoryError = ensureDirectory(existing.absolutePath()))
            return *directoryError;
        if (!QFile::rename(incomingPath, absolutePath))
        {
            const QFileInfo raced{absolutePath};
            if (!raced.exists() || static_cast<std::uint64_t>(raced.size()) != size ||
                !QFile::remove(incomingPath))
                return error(QStringLiteral("Commit incoming mail vault object"), absolutePath);
        }
        QFile::setPermissions(absolutePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        return MailVaultObject{
            .contentHash = hash.toStdString(), .relativePath = relativePath, .size = size};
    }

    MailVaultLease::MailVaultLease(std::shared_ptr<State> state) : m_state(std::move(state))
    {
    }

    MailVaultLease::~MailVaultLease() = default;

    bool MailVaultLease::isValid() const
    {
        return m_state != nullptr && m_state->token != nullptr && m_state->token->active.load();
    }

    const MailVaultObject& MailVaultLease::object() const
    {
        return m_state->object;
    }

    std::variant<QByteArray, MailVaultError> MailVaultLease::read() const
    {
        if (!isValid())
            return error(QStringLiteral("Read mail vault object"),
                         QStringLiteral("The vault lease is no longer active."));
        QFile file{m_state->path};
        if (!file.open(QIODevice::ReadOnly))
            return error(QStringLiteral("Open mail vault object"), file.errorString());
        const auto payload = file.readAll();
        if (file.error() != QFileDevice::NoError)
            return error(QStringLiteral("Read mail vault object"), file.errorString());
        const auto hash = QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();
        if (hash.toStdString() != m_state->object.contentHash)
            return error(QStringLiteral("Verify mail vault object"), m_state->path);
        return payload;
    }

    std::variant<MailVaultLease, MailVaultError>
    MailVault::acquireLease(const MailVaultObject& object) const
    {
        const QString path = objectPath(object);
        auto token = std::make_shared<LeaseToken>();
        token->path = path.toStdString();
        if (!leaseRegistry().acquire(token->path, token,
                                     [&path] { return QFileInfo::exists(path); }))
            return error(QStringLiteral("Open mail vault object"), path);
        auto state = std::make_shared<MailVaultLease::State>();
        state->object = object;
        state->path = path;
        state->token = std::move(token);
        return MailVaultLease{std::move(state)};
    }

    bool MailVault::isLeased(const MailVaultObject& object) const
    {
        return leaseRegistry().isLeased(objectPath(object).toStdString());
    }

    std::variant<QByteArray, MailVaultError> MailVault::read(const MailVaultObject& object) const
    {
        auto leaseResult = acquireLease(object);
        if (const auto* failure = std::get_if<MailVaultError>(&leaseResult))
            return *failure;
        return std::get<MailVaultLease>(std::move(leaseResult)).read();
    }

    std::optional<MailVaultError> MailVault::evict(const MailVaultObject& object) const
    {
        const QString path = objectPath(object);
        const auto eviction =
            leaseRegistry().evict(path.toStdString(), [&path]
                                  { return !QFileInfo::exists(path) || QFile::remove(path); });
        if (!eviction.has_value())
            return error(QStringLiteral("Evict mail vault object"),
                         QStringLiteral("The object is still in use."));
        if (!*eviction)
            return error(QStringLiteral("Evict mail vault object"), path);
        return std::nullopt;
    }

    void MailVault::releaseAllLeases()
    {
        leaseRegistry().releaseAll();
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
