#pragma once

#include <QSqlDatabase>
#include <QString>
#include <Qt>

#include <chrono>
#include <optional>
#include <span>
#include <string_view>
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
        std::chrono::milliseconds busyTimeout = std::chrono::seconds{5};
    };

    struct ThreadConnectionFactoryOptions
    {
        QString connectionNamePrefix;
        QString databasePath;
        std::chrono::milliseconds busyTimeout = std::chrono::seconds{30};
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
        [[nodiscard]] const QString& connectionName() const;
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

    class DatabaseTransaction
    {
      public:
        DatabaseTransaction(const DatabaseTransaction&) = delete;
        DatabaseTransaction& operator=(const DatabaseTransaction&) = delete;
        DatabaseTransaction(DatabaseTransaction&& other) noexcept;
        DatabaseTransaction& operator=(DatabaseTransaction&& other) noexcept;
        ~DatabaseTransaction();

        [[nodiscard]] static std::variant<DatabaseTransaction, DatabaseError>
        begin(DatabaseConnection& connection, QString operation);

        [[nodiscard]] std::optional<DatabaseError> commit();
        void rollback();
        [[nodiscard]] bool isActive() const;
        [[nodiscard]] DatabaseConnection& connection() const;

      private:
        explicit DatabaseTransaction(DatabaseConnection& connection);

        DatabaseConnection* m_connection = nullptr;
        bool m_active = false;
    };

    class SerializedDatabaseWrite final
    {
      public:
        SerializedDatabaseWrite();
        SerializedDatabaseWrite(const SerializedDatabaseWrite&) = delete;
        SerializedDatabaseWrite& operator=(const SerializedDatabaseWrite&) = delete;
        SerializedDatabaseWrite(SerializedDatabaseWrite&&) = delete;
        SerializedDatabaseWrite& operator=(SerializedDatabaseWrite&&) = delete;
        ~SerializedDatabaseWrite();
    };

    class ThreadConnectionFactory
    {
      public:
        explicit ThreadConnectionFactory(ThreadConnectionFactoryOptions options);

        [[nodiscard]] static QString currentThreadTag();
        [[nodiscard]] std::variant<DatabaseConnection, DatabaseError>
        openForCurrentThread(std::string_view ownerTag) const;

      private:
        [[nodiscard]] QString makeConnectionName(std::string_view ownerTag) const;

        ThreadConnectionFactoryOptions m_options;
    };

    [[nodiscard]] MigrationRunner createDefaultMigrationRunner();

} // namespace javelin::jmap::cache
