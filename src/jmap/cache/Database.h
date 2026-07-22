#pragma once

#include <QSqlDatabase>
#include <QString>
#include <Qt>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

class QSqlError;

namespace javelin::jmap::cache
{

    class DatabaseWriteScope;

    enum class DatabaseErrorCode
    {
        DriverUnavailable,
        OpenFailed,
        QueryFailed,
        MigrationFailed,
        TransientContention,
        ThreadAffinityViolation,
    };

    struct DatabaseError
    {
        DatabaseErrorCode code;
        QString message;
    };

    [[nodiscard]] DatabaseError
    databaseError(const QString& operation, const QSqlError& error,
                  DatabaseErrorCode fallback = DatabaseErrorCode::QueryFailed);

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
        friend class DatabaseWriteScope;

        DatabaseConnection(QString connectionName, QSqlDatabase database,
                           std::shared_ptr<std::recursive_mutex> writeMutex);

        void reset();

        QString m_connectionName;
        QSqlDatabase m_database;
        std::shared_ptr<std::recursive_mutex> m_writeMutex;
        Qt::HANDLE m_ownerThread = nullptr;
    };

    class DatabaseWriteScope final
    {
      public:
        explicit DatabaseWriteScope(DatabaseConnection& connection);
        explicit DatabaseWriteScope(const QString& databasePath);
        DatabaseWriteScope(const DatabaseWriteScope&) = delete;
        DatabaseWriteScope& operator=(const DatabaseWriteScope&) = delete;
        DatabaseWriteScope(DatabaseWriteScope&&) noexcept = default;
        DatabaseWriteScope& operator=(DatabaseWriteScope&&) noexcept = default;
        ~DatabaseWriteScope();

      private:
        std::shared_ptr<std::recursive_mutex> m_mutex;
        std::unique_lock<std::recursive_mutex> m_lock;
        QString m_owner;
        std::chrono::steady_clock::time_point m_acquiredAt;
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
        DatabaseTransaction(DatabaseConnection& connection, DatabaseWriteScope writeScope);

        DatabaseConnection* m_connection = nullptr;
        std::optional<DatabaseWriteScope> m_writeScope;
        bool m_active = false;
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
