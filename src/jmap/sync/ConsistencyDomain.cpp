#include "jmap/sync/ConsistencyDomain.h"

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

    ConsistencyDomainRepository::ConsistencyDomainRepository(
        javelin::jmap::cache::DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::variant<RefreshFence, javelin::jmap::cache::DatabaseError>
    ConsistencyDomainRepository::captureRefresh(const ConsistencyDomain& domain) const
    {
        const auto generation = mutationGeneration(domain);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&generation))
        {
            return *error;
        }

        return RefreshFence{
            .domain = domain,
            .mutationGeneration = std::get<std::uint64_t>(generation),
        };
    }

    std::variant<bool, javelin::jmap::cache::DatabaseError>
    ConsistencyDomainRepository::isCurrent(const RefreshFence& fence) const
    {
        const auto generation = mutationGeneration(fence.domain);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&generation))
        {
            return *error;
        }

        return std::get<std::uint64_t>(generation) == fence.mutationGeneration;
    }

    std::variant<bool, javelin::jmap::cache::DatabaseError>
    ConsistencyDomainRepository::canCommitRefresh(const RefreshFence& fence) const
    {
        const auto current = isCurrent(fence);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&current))
        {
            return *error;
        }
        if (!std::get<bool>(current))
        {
            return false;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT EXISTS(SELECT 1 FROM mutation_journal WHERE account_id=:account_id "
            "AND data_type=:data_type AND status IN "
            "('pending','in_flight','accepted','unknown'))"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(fence.domain.accountId));
        query.bindValue(QStringLiteral(":data_type"),
                        QString::fromStdString(fence.domain.dataType));
        if (!query.exec() || !query.next())
        {
            return queryError(QStringLiteral("Check active consistency-domain mutations"), query);
        }
        return query.value(0).toInt() == 0;
    }

    std::variant<std::uint64_t, javelin::jmap::cache::DatabaseError>
    ConsistencyDomainRepository::advanceMutation(const ConsistencyDomain& domain)
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }
        if (domain.accountId.empty() || domain.dataType.empty())
        {
            return javelin::jmap::cache::DatabaseError{
                .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                .message =
                    QStringLiteral("A consistency domain requires an account and data type."),
            };
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO consistency_domains (account_id,data_type,mutation_generation) "
            "VALUES (:account_id,:data_type,1) "
            "ON CONFLICT(account_id,data_type) DO UPDATE SET "
            "mutation_generation=mutation_generation+1,updated_at=CURRENT_TIMESTAMP"));
        query.bindValue(QStringLiteral(":account_id"), QString::fromStdString(domain.accountId));
        query.bindValue(QStringLiteral(":data_type"), QString::fromStdString(domain.dataType));
        if (!query.exec())
        {
            return queryError(QStringLiteral("Advance consistency-domain generation"), query);
        }

        return mutationGeneration(domain);
    }

    std::variant<std::uint64_t, javelin::jmap::cache::DatabaseError>
    ConsistencyDomainRepository::mutationGeneration(const ConsistencyDomain& domain) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("SELECT mutation_generation FROM consistency_domains "
                                     "WHERE account_id=:account_id AND data_type=:data_type"));
        query.bindValue(QStringLiteral(":account_id"), QString::fromStdString(domain.accountId));
        query.bindValue(QStringLiteral(":data_type"), QString::fromStdString(domain.dataType));
        if (!query.exec())
        {
            return queryError(QStringLiteral("Read consistency-domain generation"), query);
        }
        if (!query.next())
        {
            return std::uint64_t{0};
        }

        return query.value(0).toULongLong();
    }

} // namespace javelin::jmap::sync
