#pragma once

#include <QSqlDatabase>
#include <QString>

#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    enum class DatabaseErrorCode
    {
        DriverUnavailable,
        OpenFailed,
        QueryFailed,
        MigrationFailed,
    };

    struct DatabaseError
    {
        DatabaseErrorCode code;
        QString message;
    };

    struct MigrationStep
    {
        int version = 0;
        QString name;
        std::vector<QString> statements;
    };

    struct AppliedMigration
    {
        int version = 0;
        QString name;
    };

    class MigrationRunner
    {
      public:
        explicit MigrationRunner(std::vector<MigrationStep> steps);

        [[nodiscard]] std::optional<DatabaseError> migrate(QSqlDatabase& database) const;
        [[nodiscard]] int latestVersion() const;
        [[nodiscard]] std::span<const MigrationStep> steps() const;

      private:
        std::vector<MigrationStep> m_steps;
    };

    struct DatabaseConnectionOptions
    {
        QString connectionName;
        QString databasePath;
    };

    class DatabaseConnection
    {
      public:
        DatabaseConnection();
        DatabaseConnection(const DatabaseConnection&) = delete;
        DatabaseConnection& operator=(const DatabaseConnection&) = delete;
        DatabaseConnection(DatabaseConnection&& other) noexcept;
        DatabaseConnection& operator=(DatabaseConnection&& other) noexcept;
        ~DatabaseConnection();

        [[nodiscard]] static std::variant<DatabaseConnection, DatabaseError>
        open(const DatabaseConnectionOptions& options);

        [[nodiscard]] QSqlDatabase& database();
        [[nodiscard]] const QSqlDatabase& database() const;
        [[nodiscard]] std::optional<DatabaseError> validate() const;
        [[nodiscard]] int schemaVersion() const;
        [[nodiscard]] std::variant<std::vector<AppliedMigration>, DatabaseError>
        appliedMigrations() const;

      private:
        DatabaseConnection(QString connectionName, QSqlDatabase database);

        void reset();

        QString m_connectionName;
        QSqlDatabase m_database;
    };

    [[nodiscard]] MigrationRunner createDefaultMigrationRunner();

} // namespace javelin::jmap::cache
