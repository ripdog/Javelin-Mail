#pragma once

#include <QString>
#include <QUuid>

#include <variant>

namespace javelin::app
{

    struct CacheLocation
    {
        QString rootPath;
        QString databasePath;
        QString vaultRootPath;
        QString searchIndexRootPath;
        QString instanceIdPath;
        QUuid instanceId;

        friend bool operator==(const CacheLocation&, const CacheLocation&) = default;
    };

    enum class CacheLocationErrorCode
    {
        InvalidRoot,
        DirectoryCreationFailed,
        IdentityReadFailed,
        IdentityWriteFailed,
        InvalidIdentity,
    };

    struct CacheLocationError
    {
        CacheLocationErrorCode code = CacheLocationErrorCode::InvalidRoot;
        QString path;
        QString detail;
    };

    using CacheLocationResult = std::variant<CacheLocation, CacheLocationError>;

    class CacheLocationProvider final
    {
      public:
        explicit CacheLocationProvider(QString rootPath);

        [[nodiscard]] static CacheLocationProvider forApplication();

        [[nodiscard]] CacheLocationResult loadOrCreate() const;
        [[nodiscard]] CacheLocationResult replaceCacheInstance() const;

      private:
        [[nodiscard]] CacheLocationResult locationFor(const QUuid& instanceId) const;
        [[nodiscard]] std::variant<QUuid, CacheLocationError> readInstanceId() const;
        [[nodiscard]] std::variant<QUuid, CacheLocationError>
        writeInstanceId(const QUuid& instanceId) const;

        QString m_rootPath;
    };

} // namespace javelin::app
