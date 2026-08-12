#include "storage/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

#include <span>
#include <utility>

namespace javelin::jmap::cache
{
    namespace
    {
        [[nodiscard]] DatabaseError makeError(const DatabaseErrorCode code,
                                              const QString& operation,
                                              const QSqlDatabase& database)
        {
            return databaseError(operation, database.lastError(), code);
        }

        [[nodiscard]] DatabaseError makeQueryError(const DatabaseErrorCode code,
                                                   const QString& operation, const QSqlQuery& query)
        {
            return databaseError(operation, query.lastError(), code);
        }

        [[nodiscard]] bool isSortedUnique(const std::span<const MigrationStep> steps)
        {
            int previousVersion = 0;
            for (const auto& step : steps)
            {
                if (step.version <= previousVersion)
                    return false;
                previousVersion = step.version;
            }
            return true;
        }

        [[nodiscard]] std::optional<DatabaseError>
        executeStatement(QSqlDatabase& database, const QString& statement, const QString& operation)
        {
            QSqlQuery query{database};
            if (!query.exec(statement))
                return makeQueryError(DatabaseErrorCode::QueryFailed, operation, query);
            return std::nullopt;
        }

        [[nodiscard]] std::optional<DatabaseError> ensureMigrationTable(QSqlDatabase& database)
        {
            return executeStatement(
                database,
                QStringLiteral("CREATE TABLE IF NOT EXISTS schema_migrations ("
                               "version INTEGER PRIMARY KEY,"
                               "name TEXT NOT NULL,"
                               "applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
                               ") STRICT"),
                QStringLiteral("Create schema_migrations"));
        }

        [[nodiscard]] std::variant<int, DatabaseError>
        readCurrentVersion(const QSqlDatabase& database)
        {
            QSqlQuery query{database};
            if (!query.exec(
                    QStringLiteral("SELECT COALESCE(MAX(version), 0) FROM schema_migrations")))
            {
                return makeQueryError(DatabaseErrorCode::QueryFailed,
                                      QStringLiteral("Read schema version"), query);
            }
            if (!query.next())
                return 0;
            return query.value(0).toInt();
        }
    } // namespace

    MigrationRunner::MigrationRunner(std::vector<MigrationStep> steps) : m_steps(std::move(steps))
    {
    }

    std::optional<DatabaseError> MigrationRunner::migrate(QSqlDatabase& database) const
    {
        if (!isSortedUnique(m_steps))
        {
            return DatabaseError{
                .code = DatabaseErrorCode::MigrationFailed,
                .message = QStringLiteral("Migration steps must have strictly increasing versions"),
            };
        }

        if (const auto error = ensureMigrationTable(database))
            return error;

        const auto currentVersionResult = readCurrentVersion(database);
        if (std::holds_alternative<DatabaseError>(currentVersionResult))
            return std::get<DatabaseError>(currentVersionResult);

        const int currentVersion = std::get<int>(currentVersionResult);
        for (const auto& step : m_steps)
        {
            if (step.version <= currentVersion)
                continue;

            if (!database.transaction())
            {
                return makeError(DatabaseErrorCode::MigrationFailed,
                                 QStringLiteral("Begin migration transaction"), database);
            }

            std::optional<DatabaseError> failure;
            for (const auto& statement : step.statements)
            {
                if (const auto error = executeStatement(
                        database, statement,
                        QStringLiteral("Apply migration ") + QString::number(step.version) +
                            QStringLiteral(" (") + step.name + QStringLiteral(")")))
                {
                    failure = error;
                    break;
                }
            }

            if (!failure.has_value())
            {
                QSqlQuery insertQuery{database};
                insertQuery.prepare(QStringLiteral(
                    "INSERT INTO schema_migrations (version, name) VALUES (:version, :name)"));
                insertQuery.bindValue(QStringLiteral(":version"), step.version);
                insertQuery.bindValue(QStringLiteral(":name"), step.name);
                if (!insertQuery.exec())
                {
                    failure =
                        makeQueryError(DatabaseErrorCode::MigrationFailed,
                                       QStringLiteral("Record schema migration"), insertQuery);
                }
            }

            if (!failure.has_value() && !database.commit())
            {
                failure = makeError(DatabaseErrorCode::MigrationFailed,
                                    QStringLiteral("Commit migration"), database);
            }

            if (failure.has_value())
            {
                database.rollback();
                return failure;
            }
        }

        return std::nullopt;
    }

    int MigrationRunner::latestVersion() const
    {
        if (m_steps.empty())
            return 0;
        return m_steps.back().version;
    }

    std::span<const MigrationStep> MigrationRunner::steps() const
    {
        return m_steps;
    }

} // namespace javelin::jmap::cache
