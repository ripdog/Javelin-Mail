#include "app/CacheLocationProvider.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

namespace javelin::app
{

    namespace
    {
        constexpr auto databaseFileName = "cache.sqlite3";
        constexpr auto vaultDirectoryName = "mail-vault/v1";
        constexpr auto searchIndexDirectoryName = "indexes";
        constexpr auto instanceIdFileName = "cache-instance-id";

        [[nodiscard]] QString pathKey(const char* value)
        {
            return QString::fromLatin1(value);
        }

        [[nodiscard]] CacheLocationError error(const CacheLocationErrorCode code,
                                               const QString& path, const QString& detail)
        {
            return {.code = code, .path = path, .detail = detail};
        }

    } // namespace

    CacheLocationProvider::CacheLocationProvider(QString rootPath)
        : m_rootPath(QDir::cleanPath(std::move(rootPath)))
    {
    }

    CacheLocationProvider CacheLocationProvider::forApplication()
    {
        return CacheLocationProvider{
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)};
    }

    CacheLocationResult CacheLocationProvider::loadOrCreate() const
    {
        if (m_rootPath.isEmpty() || m_rootPath == QStringLiteral("."))
        {
            return error(CacheLocationErrorCode::InvalidRoot, m_rootPath,
                         QStringLiteral("cache root path is empty"));
        }
        if (!QDir{}.mkpath(m_rootPath))
        {
            return error(CacheLocationErrorCode::DirectoryCreationFailed, m_rootPath,
                         QStringLiteral("could not create cache root directory"));
        }

        if (QFileInfo::exists(QDir{m_rootPath}.filePath(pathKey(instanceIdFileName))))
        {
            const auto instanceId = readInstanceId();
            if (const auto* failure = std::get_if<CacheLocationError>(&instanceId))
                return *failure;
            return locationFor(std::get<QUuid>(instanceId));
        }

        return replaceCacheInstance();
    }

    CacheLocationResult CacheLocationProvider::replaceCacheInstance() const
    {
        if (m_rootPath.isEmpty() || m_rootPath == QStringLiteral("."))
        {
            return error(CacheLocationErrorCode::InvalidRoot, m_rootPath,
                         QStringLiteral("cache root path is empty"));
        }
        if (!QDir{}.mkpath(m_rootPath))
        {
            return error(CacheLocationErrorCode::DirectoryCreationFailed, m_rootPath,
                         QStringLiteral("could not create cache root directory"));
        }

        const auto instanceId = writeInstanceId(QUuid::createUuid());
        if (const auto* failure = std::get_if<CacheLocationError>(&instanceId))
            return *failure;
        return locationFor(std::get<QUuid>(instanceId));
    }

    CacheLocationResult CacheLocationProvider::locationFor(const QUuid& instanceId) const
    {
        return CacheLocation{
            .rootPath = m_rootPath,
            .databasePath = QDir{m_rootPath}.filePath(pathKey(databaseFileName)),
            .vaultRootPath = QDir{m_rootPath}.filePath(pathKey(vaultDirectoryName)),
            .searchIndexRootPath = QDir{m_rootPath}.filePath(pathKey(searchIndexDirectoryName)),
            .instanceIdPath = QDir{m_rootPath}.filePath(pathKey(instanceIdFileName)),
            .instanceId = instanceId,
        };
    }

    std::variant<QUuid, CacheLocationError> CacheLocationProvider::readInstanceId() const
    {
        const QString path = QDir{m_rootPath}.filePath(pathKey(instanceIdFileName));
        QFile file{path};
        if (!file.open(QIODevice::ReadOnly))
        {
            return error(CacheLocationErrorCode::IdentityReadFailed, path,
                         QStringLiteral("could not read cache instance identity: ") +
                             file.errorString());
        }

        const auto instanceId = QUuid::fromString(QString::fromUtf8(file.readAll()).trimmed());
        if (instanceId.isNull())
        {
            return error(CacheLocationErrorCode::InvalidIdentity, path,
                         QStringLiteral("cache instance identity is not a UUID"));
        }
        return instanceId;
    }

    std::variant<QUuid, CacheLocationError>
    CacheLocationProvider::writeInstanceId(const QUuid& instanceId) const
    {
        const QString path = QDir{m_rootPath}.filePath(pathKey(instanceIdFileName));
        QSaveFile file{path};
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            return error(CacheLocationErrorCode::IdentityWriteFailed, path,
                         QStringLiteral("could not write cache instance identity: ") +
                             file.errorString());
        }
        if (file.write(instanceId.toString(QUuid::WithoutBraces).toUtf8()) < 0 || !file.commit())
        {
            return error(CacheLocationErrorCode::IdentityWriteFailed, path,
                         QStringLiteral("could not commit cache instance identity: ") +
                             file.errorString());
        }
        return instanceId;
    }

} // namespace javelin::app
