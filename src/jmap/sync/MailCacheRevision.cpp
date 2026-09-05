#include "jmap/sync/MailCacheRevision.h"

#include <QSqlError>
#include <QSqlQuery>

namespace javelin::jmap::sync
{
    namespace
    {
        [[nodiscard]] javelin::jmap::cache::DatabaseError queryError(const QString& operation,
                                                                     const QSqlQuery& query)
        {
            return {
                .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                .message = operation + QStringLiteral(": ") + query.lastError().text(),
            };
        }
    } // namespace

    MailCacheRevisionRepository::MailCacheRevisionRepository(
        javelin::jmap::cache::DatabaseConnection& databaseConnection)
        : m_databaseConnection(databaseConnection)
    {
    }

    std::variant<MailCacheRevisionFence, javelin::jmap::cache::DatabaseError>
    MailCacheRevisionRepository::capture(std::string accountId) const
    {
        const auto revision = read(accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&revision))
            return *error;
        return MailCacheRevisionFence{
            .accountId = std::move(accountId),
            .revision = std::get<std::uint64_t>(revision),
        };
    }

    std::variant<bool, javelin::jmap::cache::DatabaseError>
    MailCacheRevisionRepository::isCurrent(const MailCacheRevisionFence& fence) const
    {
        const auto revision = read(fence.accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&revision))
            return *error;
        return std::get<std::uint64_t>(revision) == fence.revision;
    }

    std::variant<bool, javelin::jmap::cache::DatabaseError>
    MailCacheRevisionRepository::advanceIfCurrent(
        javelin::jmap::cache::DatabaseTransaction& transaction,
        const MailCacheRevisionFence& fence) const
    {
        QSqlQuery query{transaction.connection().database()};
        query.prepare(QStringLiteral(
            "INSERT INTO mail_cache_revisions(account_id,revision) VALUES(:account_id,1) "
            "ON CONFLICT(account_id) DO UPDATE SET revision=revision+1,"
            "updated_at=CURRENT_TIMESTAMP WHERE revision=:expected_revision"));
        query.bindValue(QStringLiteral(":account_id"), QString::fromStdString(fence.accountId));
        query.bindValue(QStringLiteral(":expected_revision"),
                        static_cast<qulonglong>(fence.revision));
        if (!query.exec())
            return queryError(QStringLiteral("Advance mail cache revision conditionally"), query);
        return query.numRowsAffected() == 1;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailCacheRevisionRepository::advance(javelin::jmap::cache::DatabaseTransaction& transaction,
                                         const std::string_view accountId) const
    {
        QSqlQuery query{transaction.connection().database()};
        query.prepare(QStringLiteral(
            "INSERT INTO mail_cache_revisions(account_id,revision) VALUES(:account_id,1) "
            "ON CONFLICT(account_id) DO UPDATE SET revision=revision+1,"
            "updated_at=CURRENT_TIMESTAMP"));
        query.bindValue(QStringLiteral(":account_id"), QString::fromUtf8(accountId));
        if (!query.exec())
            return queryError(QStringLiteral("Advance mail cache revision"), query);
        return std::nullopt;
    }

    std::variant<std::uint64_t, javelin::jmap::cache::DatabaseError>
    MailCacheRevisionRepository::read(const std::string_view accountId) const
    {
        if (const auto error = m_databaseConnection.validate())
            return *error;
        QSqlQuery query{m_databaseConnection.database()};
        query.prepare(QStringLiteral(
            "SELECT revision FROM mail_cache_revisions WHERE account_id=:account_id"));
        query.bindValue(QStringLiteral(":account_id"), QString::fromUtf8(accountId));
        if (!query.exec())
            return queryError(QStringLiteral("Read mail cache revision"), query);
        if (!query.next())
            return std::uint64_t{0};
        return query.value(0).toULongLong();
    }
} // namespace javelin::jmap::sync
