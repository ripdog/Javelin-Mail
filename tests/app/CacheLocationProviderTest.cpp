#include "app/CacheLocationProvider.h"

#include <catch2/catch_test_macros.hpp>

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <variant>

namespace
{

    using javelin::app::CacheLocation;
    using javelin::app::CacheLocationError;
    using javelin::app::CacheLocationProvider;

    [[nodiscard]] CacheLocation requireLocation(const auto& result)
    {
        if (const auto* error = std::get_if<CacheLocationError>(&result))
            FAIL(error->detail.toStdString());
        return std::get<CacheLocation>(result);
    }

} // namespace

TEST_CASE("cache location provider creates identity without opening SQLite", "[app][cache]")
{
    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());

    const QString rootPath = temporaryDir.filePath(QStringLiteral("cache-root"));
    const CacheLocationProvider provider{rootPath};
    const auto first = requireLocation(provider.loadOrCreate());

    CHECK(first.rootPath == rootPath);
    CHECK(first.databasePath == temporaryDir.filePath(QStringLiteral("cache-root/cache.sqlite3")));
    CHECK(first.vaultRootPath == temporaryDir.filePath(QStringLiteral("cache-root/mail-vault/v1")));
    CHECK(first.searchIndexRootPath == temporaryDir.filePath(QStringLiteral("cache-root/indexes")));
    CHECK_FALSE(first.instanceId.isNull());
    CHECK(QFileInfo::exists(first.instanceIdPath));
    CHECK_FALSE(QFileInfo::exists(first.databasePath));

    const auto second = requireLocation(provider.loadOrCreate());
    CHECK(second == first);
}

TEST_CASE("cache replacement receives a new persistent instance identity", "[app][cache]")
{
    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());

    const CacheLocationProvider provider{temporaryDir.filePath(QStringLiteral("cache-root"))};
    const auto before = requireLocation(provider.loadOrCreate());
    const auto after = requireLocation(provider.replaceCacheInstance());

    CHECK(after.rootPath == before.rootPath);
    CHECK(after.databasePath == before.databasePath);
    CHECK(after.instanceIdPath == before.instanceIdPath);
    CHECK(after.instanceId != before.instanceId);
    CHECK(requireLocation(provider.loadOrCreate()) == after);
}

TEST_CASE("cache location provider rejects a malformed instance identity", "[app][cache]")
{
    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());

    const QString rootPath = temporaryDir.filePath(QStringLiteral("cache-root"));
    const CacheLocationProvider provider{rootPath};
    const auto created = requireLocation(provider.loadOrCreate());
    QFile identity{created.instanceIdPath};
    REQUIRE(identity.open(QIODevice::WriteOnly | QIODevice::Truncate));
    REQUIRE(identity.write(QByteArrayLiteral("not-a-uuid")) > 0);
    identity.close();

    const auto result = provider.loadOrCreate();
    const auto* error = std::get_if<CacheLocationError>(&result);
    REQUIRE(error != nullptr);
    CHECK(error->code == javelin::app::CacheLocationErrorCode::InvalidIdentity);
}
