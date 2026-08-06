#include "jmap/cache/Database.h"

#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <sstream>
#include <utility>

namespace javelin::jmap::cache
{

    Q_LOGGING_CATEGORY(logDatabaseAccess, "jmap.cache.database")

    DatabaseError databaseError(const QString& operation, const QSqlError& error,
                                const DatabaseErrorCode fallback)
    {
        bool ok = false;
        const int nativeCode = error.nativeErrorCode().toInt(&ok);
        const auto code = ok && (nativeCode & 0xff) >= 5 && (nativeCode & 0xff) <= 6
                              ? DatabaseErrorCode::TransientContention
                              : fallback;
        return {
            .code = code,
            .message = operation + QStringLiteral(": ") + error.text(),
        };
    }

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
                {
                    return false;
                }

                previousVersion = step.version;
            }

            return true;
        }

        [[nodiscard]] std::optional<DatabaseError>
        executeStatement(QSqlDatabase& database, const QString& statement, const QString& operation)
        {
            QSqlQuery query{database};
            if (!query.exec(statement))
            {
                return makeQueryError(DatabaseErrorCode::QueryFailed, operation, query);
            }

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
            {
                return 0;
            }

            return query.value(0).toInt();
        }

        [[nodiscard]] QString databaseIdentity(const QString& databasePath)
        {
            if (databasePath == QStringLiteral(":memory:") ||
                databasePath.startsWith(QStringLiteral("file:")))
                return databasePath;
            return QDir::cleanPath(QFileInfo(databasePath).absoluteFilePath());
        }

        [[nodiscard]] std::shared_ptr<std::recursive_mutex>
        writeMutexForDatabase(const QString& databasePath)
        {
            static std::mutex registryMutex;
            static std::map<QString, std::weak_ptr<std::recursive_mutex>> registry;
            const std::scoped_lock lock{registryMutex};
            auto& weak = registry[databaseIdentity(databasePath)];
            auto mutex = weak.lock();
            if (!mutex)
            {
                mutex = std::make_shared<std::recursive_mutex>();
                weak = mutex;
            }
            return mutex;
        }

        [[nodiscard]] std::optional<DatabaseError>
        applyPragmas(QSqlDatabase& database, const std::chrono::milliseconds busyTimeout)
        {
            const std::vector<std::pair<QString, QString>> pragmas{
                {QStringLiteral("Enable foreign keys"), QStringLiteral("PRAGMA foreign_keys = ON")},
                {QStringLiteral("Enable WAL"), QStringLiteral("PRAGMA journal_mode = WAL")},
                {QStringLiteral("Reduce fsync pressure"),
                 QStringLiteral("PRAGMA synchronous = NORMAL")},
                {QStringLiteral("Configure busy timeout"),
                 QStringLiteral("PRAGMA busy_timeout = %1")
                     .arg(std::max<std::int64_t>(0, busyTimeout.count()))},
            };

            for (const auto& [operation, statement] : pragmas)
            {
                if (const auto error = executeStatement(database, statement, operation))
                {
                    return error;
                }
            }

            return std::nullopt;
        }

        [[nodiscard]] std::optional<DatabaseError>
        applyReadOnlyPragmas(QSqlDatabase& database, const std::chrono::milliseconds busyTimeout)
        {
            const std::vector<std::pair<QString, QString>> pragmas{
                {QStringLiteral("Enable query-only mode"),
                 QStringLiteral("PRAGMA query_only = ON")},
                {QStringLiteral("Configure busy timeout"),
                 QStringLiteral("PRAGMA busy_timeout = %1")
                     .arg(std::max<std::int64_t>(0, busyTimeout.count()))},
            };

            for (const auto& [operation, statement] : pragmas)
            {
                if (const auto error = executeStatement(database, statement, operation))
                    return error;
            }

            QSqlQuery query{database};
            if (!query.exec(QStringLiteral("PRAGMA query_only")) || !query.next() ||
                query.value(0).toInt() != 1)
            {
                return makeQueryError(DatabaseErrorCode::QueryFailed,
                                      QStringLiteral("Verify query-only mode"), query);
            }
            return std::nullopt;
        }

        [[nodiscard]] std::variant<std::uint64_t, DatabaseError>
        readDataVersion(const QSqlDatabase& database)
        {
            QSqlQuery query{database};
            if (!query.exec(QStringLiteral("PRAGMA data_version")) || !query.next())
            {
                return makeQueryError(DatabaseErrorCode::QueryFailed,
                                      QStringLiteral("Read SQLite data version"), query);
            }

            bool ok = false;
            const auto version = query.value(0).toULongLong(&ok);
            if (!ok)
            {
                return DatabaseError{
                    .code = DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("SQLite data version is not numeric"),
                };
            }
            return version;
        }

        [[nodiscard]] QString encodeThreadHandle(const Qt::HANDLE threadHandle)
        {
            std::ostringstream stream;
            stream << reinterpret_cast<std::uintptr_t>(threadHandle);
            return QString::fromStdString(stream.str());
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
        {
            return error;
        }

        const auto currentVersionResult = readCurrentVersion(database);
        if (std::holds_alternative<DatabaseError>(currentVersionResult))
        {
            return std::get<DatabaseError>(currentVersionResult);
        }

        const int currentVersion = std::get<int>(currentVersionResult);
        for (const auto& step : m_steps)
        {
            if (step.version <= currentVersion)
            {
                continue;
            }

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
        {
            return 0;
        }

        return m_steps.back().version;
    }

    std::span<const MigrationStep> MigrationRunner::steps() const
    {
        return m_steps;
    }

    DatabaseConnection::DatabaseConnection() = default;

    DatabaseConnection::DatabaseConnection(QString connectionName, QSqlDatabase database,
                                           std::shared_ptr<std::recursive_mutex> writeMutex)
        : m_connectionName(std::move(connectionName)), m_database(std::move(database)),
          m_writeMutex(std::move(writeMutex)), m_ownerThread(QThread::currentThreadId())
    {
    }

    DatabaseConnection::DatabaseConnection(DatabaseConnection&& other) noexcept
        : m_connectionName(std::move(other.m_connectionName)),
          m_database(std::move(other.m_database)), m_writeMutex(std::move(other.m_writeMutex)),
          m_ownerThread(std::exchange(other.m_ownerThread, nullptr))
    {
        other.m_database = QSqlDatabase{};
        other.m_connectionName.clear();
    }

    DatabaseConnection& DatabaseConnection::operator=(DatabaseConnection&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }

        reset();
        m_connectionName = std::move(other.m_connectionName);
        m_database = std::move(other.m_database);
        m_writeMutex = std::move(other.m_writeMutex);
        m_ownerThread = std::exchange(other.m_ownerThread, nullptr);
        other.m_database = QSqlDatabase{};
        other.m_connectionName.clear();
        return *this;
    }

    DatabaseConnection::~DatabaseConnection()
    {
        reset();
    }

    DatabaseWriteScope::DatabaseWriteScope(DatabaseConnection& connection)
        : m_mutex(connection.m_writeMutex), m_lock(*m_mutex, std::defer_lock),
          m_owner(connection.connectionName())
    {
        const auto startedAt = std::chrono::steady_clock::now();
        m_lock.lock();
        m_acquiredAt = std::chrono::steady_clock::now();
        const auto wait =
            std::chrono::duration_cast<std::chrono::milliseconds>(m_acquiredAt - startedAt);
        if (wait >= std::chrono::milliseconds{100})
            qCWarning(logDatabaseAccess).noquote()
                << "Database writer waited" << wait.count() << "ms" << m_owner;
    }

    DatabaseWriteScope::DatabaseWriteScope(const QString& databasePath)
        : m_mutex(writeMutexForDatabase(databasePath)), m_lock(*m_mutex, std::defer_lock),
          m_owner(QFileInfo(databasePath).fileName())
    {
        const auto startedAt = std::chrono::steady_clock::now();
        m_lock.lock();
        m_acquiredAt = std::chrono::steady_clock::now();
        const auto wait =
            std::chrono::duration_cast<std::chrono::milliseconds>(m_acquiredAt - startedAt);
        if (wait >= std::chrono::milliseconds{100})
            qCWarning(logDatabaseAccess).noquote()
                << "Database writer waited" << wait.count() << "ms" << m_owner;
    }

    DatabaseWriteScope::~DatabaseWriteScope()
    {
        if (!m_lock.owns_lock())
            return;
        const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_acquiredAt);
        if (held >= std::chrono::seconds{1})
            qCWarning(logDatabaseAccess).noquote()
                << "Database writer held lease" << held.count() << "ms" << m_owner;
    }

    DatabaseTransaction::DatabaseTransaction(DatabaseConnection& connection,
                                             DatabaseWriteScope writeScope)
        : m_connection(&connection), m_writeScope(std::move(writeScope)), m_active(true)
    {
    }

    DatabaseTransaction::DatabaseTransaction(DatabaseTransaction&& other) noexcept
        : m_connection(std::exchange(other.m_connection, nullptr)),
          m_writeScope(std::move(other.m_writeScope)),
          m_active(std::exchange(other.m_active, false))
    {
    }

    DatabaseTransaction& DatabaseTransaction::operator=(DatabaseTransaction&& other) noexcept
    {
        if (this != &other)
        {
            rollback();
            m_connection = std::exchange(other.m_connection, nullptr);
            m_writeScope = std::move(other.m_writeScope);
            m_active = std::exchange(other.m_active, false);
        }
        return *this;
    }

    DatabaseTransaction::~DatabaseTransaction()
    {
        rollback();
    }

    std::variant<DatabaseTransaction, DatabaseError>
    DatabaseTransaction::begin(DatabaseConnection& connection, QString operation)
    {
        if (const auto error = connection.validate())
        {
            return *error;
        }
        DatabaseWriteScope writeScope{connection};
        QSqlQuery begin{connection.database()};
        if (!begin.exec(QStringLiteral("BEGIN IMMEDIATE TRANSACTION")))
        {
            return makeQueryError(DatabaseErrorCode::QueryFailed, std::move(operation), begin);
        }
        return DatabaseTransaction{connection, std::move(writeScope)};
    }

    std::optional<DatabaseError> DatabaseTransaction::commit()
    {
        if (!m_active || m_connection == nullptr)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Commit inactive database transaction"),
            };
        }
        if (!m_connection->database().commit())
        {
            const auto error =
                makeError(DatabaseErrorCode::QueryFailed,
                          QStringLiteral("Commit database transaction"), m_connection->database());
            m_connection->database().rollback();
            m_active = false;
            m_writeScope.reset();
            return error;
        }
        m_active = false;
        m_writeScope.reset();
        return std::nullopt;
    }

    void DatabaseTransaction::rollback()
    {
        if (m_active && m_connection != nullptr)
        {
            m_connection->database().rollback();
            m_active = false;
            m_writeScope.reset();
        }
    }

    bool DatabaseTransaction::isActive() const
    {
        return m_active;
    }

    DatabaseConnection& DatabaseTransaction::connection() const
    {
        return *m_connection;
    }

    std::variant<DatabaseConnection, DatabaseError>
    DatabaseConnection::open(const DatabaseConnectionOptions& options)
    {
        if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")))
        {
            return DatabaseError{
                .code = DatabaseErrorCode::DriverUnavailable,
                .message = QStringLiteral("Qt SQLite driver is not available"),
            };
        }

        auto writeMutex = writeMutexForDatabase(options.databasePath);
        const std::unique_lock writeLock{*writeMutex};
        QSqlDatabase database =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), options.connectionName);
        const auto discardConnection = [&database, &options]()
        {
            database.close();
            database = QSqlDatabase{};
            QSqlDatabase::removeDatabase(options.connectionName);
        };
        database.setDatabaseName(options.databasePath);
        if (!database.open())
        {
            const auto error =
                makeError(DatabaseErrorCode::OpenFailed, QStringLiteral("Open database"), database);
            discardConnection();
            return error;
        }

        if (const auto pragmaError = applyPragmas(database, options.busyTimeout))
        {
            discardConnection();
            return *pragmaError;
        }

        const auto migrationRunner = createDefaultMigrationRunner();
        if (const auto migrationError = migrationRunner.migrate(database))
        {
            discardConnection();
            return *migrationError;
        }

        return DatabaseConnection{options.connectionName, database, std::move(writeMutex)};
    }

    QSqlDatabase& DatabaseConnection::database()
    {
        Q_ASSERT_X(m_ownerThread == QThread::currentThreadId(), "DatabaseConnection::database",
                   "A database connection was accessed from a thread that does not own it");
        return m_database;
    }

    const QSqlDatabase& DatabaseConnection::database() const
    {
        Q_ASSERT_X(m_ownerThread == QThread::currentThreadId(), "DatabaseConnection::database",
                   "A database connection was accessed from a thread that does not own it");
        return m_database;
    }

    const QString& DatabaseConnection::connectionName() const
    {
        return m_connectionName;
    }

    std::optional<DatabaseError> DatabaseConnection::validate() const
    {
        if (m_ownerThread != nullptr && m_ownerThread != QThread::currentThreadId())
        {
            return DatabaseError{
                .code = DatabaseErrorCode::ThreadAffinityViolation,
                .message = QStringLiteral(
                    "Database connection accessed from a thread that does not own it"),
            };
        }

        if (!m_database.isValid() || !m_database.isOpen())
        {
            return DatabaseError{
                .code = DatabaseErrorCode::OpenFailed,
                .message = QStringLiteral("Database connection is not open"),
            };
        }

        return std::nullopt;
    }

    int DatabaseConnection::schemaVersion() const
    {
        if (const auto error = validate())
        {
            Q_UNUSED(error);
            return 0;
        }

        auto versionResult = readCurrentVersion(m_database);
        if (std::holds_alternative<DatabaseError>(versionResult))
        {
            return 0;
        }

        return std::get<int>(versionResult);
    }

    std::variant<std::uint64_t, DatabaseError> DatabaseConnection::dataVersion() const
    {
        if (const auto error = validate())
            return *error;
        return readDataVersion(m_database);
    }

    std::variant<std::vector<AppliedMigration>, DatabaseError>
    DatabaseConnection::appliedMigrations() const
    {
        if (const auto error = validate())
        {
            return *error;
        }

        QSqlQuery query{m_database};
        if (!query.exec(
                QStringLiteral("SELECT version, name FROM schema_migrations ORDER BY version")))
        {
            return makeQueryError(DatabaseErrorCode::QueryFailed,
                                  QStringLiteral("Read schema_migrations"), query);
        }

        std::vector<AppliedMigration> migrations;
        while (query.next())
        {
            migrations.push_back(AppliedMigration{
                .version = query.value(0).toInt(),
                .name = query.value(1).toString(),
            });
        }

        return migrations;
    }

    void DatabaseConnection::reset()
    {
        if (m_connectionName.isEmpty())
        {
            return;
        }

        const QString connectionName = std::move(m_connectionName);
        if (m_database.isValid())
        {
            m_database.close();
        }

        m_database = QSqlDatabase{};
        m_writeMutex.reset();
        m_ownerThread = nullptr;
        QSqlDatabase::removeDatabase(connectionName);
    }

    ReadOnlyDatabaseConnection::ReadOnlyDatabaseConnection() = default;

    ReadOnlyDatabaseConnection::ReadOnlyDatabaseConnection(QString connectionName,
                                                           QSqlDatabase database)
        : m_connectionName(std::move(connectionName)), m_database(std::move(database)),
          m_ownerThread(QThread::currentThreadId())
    {
    }

    ReadOnlyDatabaseConnection::ReadOnlyDatabaseConnection(
        ReadOnlyDatabaseConnection&& other) noexcept
        : m_connectionName(std::move(other.m_connectionName)),
          m_database(std::move(other.m_database)),
          m_ownerThread(std::exchange(other.m_ownerThread, nullptr))
    {
        other.m_database = QSqlDatabase{};
        other.m_connectionName.clear();
    }

    ReadOnlyDatabaseConnection&
    ReadOnlyDatabaseConnection::operator=(ReadOnlyDatabaseConnection&& other) noexcept
    {
        if (this == &other)
            return *this;

        reset();
        m_connectionName = std::move(other.m_connectionName);
        m_database = std::move(other.m_database);
        m_ownerThread = std::exchange(other.m_ownerThread, nullptr);
        other.m_database = QSqlDatabase{};
        other.m_connectionName.clear();
        return *this;
    }

    ReadOnlyDatabaseConnection::~ReadOnlyDatabaseConnection()
    {
        reset();
    }

    std::variant<ReadOnlyDatabaseConnection, DatabaseError>
    ReadOnlyDatabaseConnection::open(const ReadOnlyDatabaseConnectionOptions& options)
    {
        if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")))
        {
            return DatabaseError{
                .code = DatabaseErrorCode::DriverUnavailable,
                .message = QStringLiteral("Qt SQLite driver is not available"),
            };
        }
        if (options.connectionName.isEmpty() || options.databasePath.isEmpty())
        {
            return DatabaseError{
                .code = DatabaseErrorCode::OpenFailed,
                .message = QStringLiteral("Read-only database connection requires a name and path"),
            };
        }

        QSqlDatabase database =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), options.connectionName);
        const auto discardConnection = [&database, &options]()
        {
            database.close();
            database = QSqlDatabase{};
            QSqlDatabase::removeDatabase(options.connectionName);
        };
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        database.setDatabaseName(options.databasePath);
        if (!database.open())
        {
            const auto error = makeError(DatabaseErrorCode::OpenFailed,
                                         QStringLiteral("Open read-only database"), database);
            discardConnection();
            return error;
        }

        if (const auto pragmaError = applyReadOnlyPragmas(database, options.busyTimeout))
        {
            discardConnection();
            return *pragmaError;
        }

        return ReadOnlyDatabaseConnection{options.connectionName, database};
    }

    const QSqlDatabase& ReadOnlyDatabaseConnection::database() const
    {
        Q_ASSERT_X(m_ownerThread == QThread::currentThreadId(),
                   "ReadOnlyDatabaseConnection::database",
                   "A database connection was accessed from a thread that does not own it");
        return m_database;
    }

    const QString& ReadOnlyDatabaseConnection::connectionName() const
    {
        return m_connectionName;
    }

    std::optional<DatabaseError> ReadOnlyDatabaseConnection::validate() const
    {
        if (m_ownerThread != nullptr && m_ownerThread != QThread::currentThreadId())
        {
            return DatabaseError{
                .code = DatabaseErrorCode::ThreadAffinityViolation,
                .message = QStringLiteral(
                    "Read-only database connection accessed from a thread that does not own it"),
            };
        }
        if (!m_database.isValid() || !m_database.isOpen())
        {
            return DatabaseError{
                .code = DatabaseErrorCode::OpenFailed,
                .message = QStringLiteral("Read-only database connection is not open"),
            };
        }
        return std::nullopt;
    }

    int ReadOnlyDatabaseConnection::schemaVersion() const
    {
        if (const auto error = validate())
        {
            Q_UNUSED(error);
            return 0;
        }

        const auto versionResult = readCurrentVersion(m_database);
        if (std::holds_alternative<DatabaseError>(versionResult))
            return 0;
        return std::get<int>(versionResult);
    }

    std::variant<std::uint64_t, DatabaseError> ReadOnlyDatabaseConnection::dataVersion() const
    {
        if (const auto error = validate())
            return *error;
        return readDataVersion(m_database);
    }

    DatabaseReadView::DatabaseReadView(const DatabaseConnection& connection)
        : m_connection(&connection)
    {
    }

    DatabaseReadView::DatabaseReadView(const ReadOnlyDatabaseConnection& connection)
        : m_connection(&connection)
    {
    }

    const QSqlDatabase& DatabaseReadView::database() const
    {
        return std::visit([](const auto* connection) -> const QSqlDatabase&
                          { return connection->database(); }, m_connection);
    }

    std::optional<DatabaseError> DatabaseReadView::validate() const
    {
        return std::visit([](const auto* connection) { return connection->validate(); },
                          m_connection);
    }

    std::variant<std::uint64_t, DatabaseError> DatabaseReadView::dataVersion() const
    {
        return std::visit([](const auto* connection) { return connection->dataVersion(); },
                          m_connection);
    }

    void ReadOnlyDatabaseConnection::reset()
    {
        if (m_connectionName.isEmpty())
            return;

        const QString connectionName = std::move(m_connectionName);
        if (m_database.isValid())
            m_database.close();
        m_database = QSqlDatabase{};
        m_ownerThread = nullptr;
        QSqlDatabase::removeDatabase(connectionName);
    }

    DaemonDatabaseFactory::DaemonDatabaseFactory(DatabaseConnectionOptions options)
        : m_options(std::move(options))
    {
    }

    std::variant<DatabaseConnection, DatabaseError> DaemonDatabaseFactory::open() const
    {
        return DatabaseConnection::open(m_options);
    }

    GuiDatabaseFactory::GuiDatabaseFactory(ReadOnlyThreadConnectionFactoryOptions options)
        : m_options(std::move(options))
    {
    }

    std::variant<ReadOnlyDatabaseConnection, DatabaseError>
    GuiDatabaseFactory::openForCurrentThread(const std::string_view ownerTag) const
    {
        return ReadOnlyDatabaseConnection::open({
            .connectionName = makeConnectionName(ownerTag),
            .databasePath = m_options.databasePath,
            .busyTimeout = m_options.busyTimeout,
        });
    }

    QString GuiDatabaseFactory::makeConnectionName(const std::string_view ownerTag) const
    {
        return QStringLiteral("%1-%2-thread-%3")
            .arg(m_options.connectionNamePrefix, QString::fromStdString(std::string{ownerTag}),
                 ReadOnlyThreadConnectionFactory::currentThreadTag());
    }

    ThreadConnectionFactory::ThreadConnectionFactory(ThreadConnectionFactoryOptions options)
        : m_options(std::move(options))
    {
    }

    QString ThreadConnectionFactory::currentThreadTag()
    {
        return encodeThreadHandle(QThread::currentThreadId());
    }

    std::variant<DatabaseConnection, DatabaseError>
    ThreadConnectionFactory::openForCurrentThread(const std::string_view ownerTag) const
    {
        return DatabaseConnection::open({
            .connectionName = makeConnectionName(ownerTag),
            .databasePath = m_options.databasePath,
            .busyTimeout = m_options.busyTimeout,
        });
    }

    QString ThreadConnectionFactory::makeConnectionName(const std::string_view ownerTag) const
    {
        return QStringLiteral("%1-%2-thread-%3")
            .arg(m_options.connectionNamePrefix, QString::fromStdString(std::string{ownerTag}),
                 currentThreadTag());
    }

    ReadOnlyThreadConnectionFactory::ReadOnlyThreadConnectionFactory(
        ReadOnlyThreadConnectionFactoryOptions options)
        : m_options(std::move(options))
    {
    }

    QString ReadOnlyThreadConnectionFactory::currentThreadTag()
    {
        return encodeThreadHandle(QThread::currentThreadId());
    }

    std::variant<ReadOnlyDatabaseConnection, DatabaseError>
    ReadOnlyThreadConnectionFactory::openForCurrentThread(const std::string_view ownerTag) const
    {
        return ReadOnlyDatabaseConnection::open({
            .connectionName = makeConnectionName(ownerTag),
            .databasePath = m_options.databasePath,
            .busyTimeout = m_options.busyTimeout,
        });
    }

    QString
    ReadOnlyThreadConnectionFactory::makeConnectionName(const std::string_view ownerTag) const
    {
        return QStringLiteral("%1-%2-thread-%3")
            .arg(m_options.connectionNamePrefix, QString::fromStdString(std::string{ownerTag}),
                 currentThreadTag());
    }

    MigrationRunner createDefaultMigrationRunner()
    {
        return MigrationRunner{
            {
                MigrationStep{
                    .version = 1,
                    .name = QStringLiteral("initial_cache_schema"),
                    .statements =
                        {
                            QStringLiteral("CREATE TABLE IF NOT EXISTS accounts ("
                                           "account_id TEXT PRIMARY KEY,"
                                           "email_address TEXT NOT NULL,"
                                           "session_url TEXT NOT NULL,"
                                           "is_primary INTEGER NOT NULL DEFAULT 0,"
                                           "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                                           "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
                                           ") STRICT"),
                            QStringLiteral(
                                "CREATE TABLE IF NOT EXISTS sessions ("
                                "account_id TEXT PRIMARY KEY REFERENCES accounts(account_id) ON "
                                "DELETE CASCADE,"
                                "api_url TEXT NOT NULL,"
                                "download_url TEXT,"
                                "upload_url TEXT,"
                                "event_source_url TEXT,"
                                "state TEXT,"
                                "username TEXT NOT NULL"
                                ") STRICT"),
                            QStringLiteral("CREATE TABLE IF NOT EXISTS mailboxes ("
                                           "account_id TEXT NOT NULL REFERENCES "
                                           "accounts(account_id) ON DELETE "
                                           "CASCADE,"
                                           "mailbox_id TEXT NOT NULL,"
                                           "parent_mailbox_id TEXT,"
                                           "name TEXT NOT NULL,"
                                           "role TEXT,"
                                           "sort_order INTEGER NOT NULL DEFAULT 0,"
                                           "total_emails INTEGER NOT NULL DEFAULT 0,"
                                           "unread_emails INTEGER NOT NULL DEFAULT 0,"
                                           "total_threads INTEGER NOT NULL DEFAULT 0,"
                                           "unread_threads INTEGER NOT NULL DEFAULT 0,"
                                           "rights_json TEXT NOT NULL DEFAULT '{}',"
                                           "state TEXT,"
                                           "PRIMARY KEY (account_id, mailbox_id)"
                                           ") STRICT"),
                            QStringLiteral("CREATE TABLE IF NOT EXISTS threads ("
                                           "account_id TEXT NOT NULL REFERENCES "
                                           "accounts(account_id) ON DELETE "
                                           "CASCADE,"
                                           "thread_id TEXT NOT NULL,"
                                           "email_ids_json TEXT NOT NULL DEFAULT '[]',"
                                           "state TEXT,"
                                           "PRIMARY KEY (account_id, thread_id)"
                                           ") STRICT"),
                            QStringLiteral("CREATE TABLE IF NOT EXISTS emails ("
                                           "account_id TEXT NOT NULL REFERENCES "
                                           "accounts(account_id) ON DELETE "
                                           "CASCADE,"
                                           "email_id TEXT NOT NULL,"
                                           "thread_id TEXT,"
                                           "blob_id TEXT,"
                                           "received_at TEXT,"
                                           "sent_at TEXT,"
                                           "subject TEXT NOT NULL DEFAULT '',"
                                           "preview TEXT NOT NULL DEFAULT '',"
                                           "mailbox_ids_json TEXT NOT NULL DEFAULT '[]',"
                                           "keywords_json TEXT NOT NULL DEFAULT '{}',"
                                           "has_attachment INTEGER NOT NULL DEFAULT 0,"
                                           "size INTEGER NOT NULL DEFAULT 0,"
                                           "state TEXT,"
                                           "PRIMARY KEY (account_id, email_id)"
                                           ") STRICT"),
                            QStringLiteral("CREATE TABLE IF NOT EXISTS email_mailboxes ("
                                           "account_id TEXT NOT NULL REFERENCES "
                                           "accounts(account_id) ON DELETE "
                                           "CASCADE,"
                                           "email_id TEXT NOT NULL,"
                                           "mailbox_id TEXT NOT NULL,"
                                           "PRIMARY KEY (account_id, email_id, mailbox_id)"
                                           ") STRICT"),
                            QStringLiteral("CREATE TABLE IF NOT EXISTS email_keywords ("
                                           "account_id TEXT NOT NULL REFERENCES "
                                           "accounts(account_id) ON DELETE "
                                           "CASCADE,"
                                           "email_id TEXT NOT NULL,"
                                           "keyword TEXT NOT NULL,"
                                           "PRIMARY KEY (account_id, email_id, keyword)"
                                           ") STRICT"),
                            QStringLiteral(
                                "CREATE TABLE IF NOT EXISTS email_addresses ("
                                "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON "
                                "DELETE "
                                "CASCADE,"
                                "email_id TEXT NOT NULL,"
                                "field_name TEXT NOT NULL,"
                                "position INTEGER NOT NULL,"
                                "display_name TEXT,"
                                "address TEXT NOT NULL,"
                                "PRIMARY KEY (account_id, email_id, field_name, position)"
                                ") STRICT"),
                            QStringLiteral("CREATE TABLE IF NOT EXISTS identities ("
                                           "account_id TEXT NOT NULL REFERENCES "
                                           "accounts(account_id) ON DELETE "
                                           "CASCADE,"
                                           "identity_id TEXT NOT NULL,"
                                           "email_address TEXT NOT NULL,"
                                           "name TEXT,"
                                           "reply_to_json TEXT NOT NULL DEFAULT '[]',"
                                           "bcc_json TEXT NOT NULL DEFAULT '[]',"
                                           "text_signature TEXT,"
                                           "html_signature TEXT,"
                                           "may_delete INTEGER NOT NULL DEFAULT 0,"
                                           "state TEXT,"
                                           "PRIMARY KEY (account_id, identity_id)"
                                           ") STRICT"),
                            QStringLiteral("CREATE TABLE IF NOT EXISTS submissions ("
                                           "account_id TEXT NOT NULL REFERENCES "
                                           "accounts(account_id) ON DELETE "
                                           "CASCADE,"
                                           "submission_id TEXT NOT NULL,"
                                           "email_id TEXT NOT NULL,"
                                           "thread_id TEXT,"
                                           "envelope_json TEXT NOT NULL DEFAULT '{}',"
                                           "undo_status TEXT,"
                                           "delivery_status TEXT,"
                                           "state TEXT,"
                                           "PRIMARY KEY (account_id, submission_id)"
                                           ") STRICT"),
                            QStringLiteral("CREATE TABLE IF NOT EXISTS sync_state ("
                                           "account_id TEXT NOT NULL REFERENCES "
                                           "accounts(account_id) ON DELETE "
                                           "CASCADE,"
                                           "object_type TEXT NOT NULL,"
                                           "query_key TEXT NOT NULL DEFAULT '',"
                                           "state_token TEXT NOT NULL,"
                                           "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                                           "PRIMARY KEY (account_id, object_type, query_key)"
                                           ") STRICT"),
                            QStringLiteral("CREATE TABLE IF NOT EXISTS pending_actions ("
                                           "pending_action_id TEXT PRIMARY KEY,"
                                           "account_id TEXT NOT NULL REFERENCES "
                                           "accounts(account_id) ON DELETE "
                                           "CASCADE,"
                                           "action_type TEXT NOT NULL,"
                                           "status TEXT NOT NULL,"
                                           "payload_json TEXT NOT NULL,"
                                           "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                                           "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
                                           ") STRICT"),
                            QStringLiteral(
                                "CREATE TABLE IF NOT EXISTS notifications ("
                                "notification_id TEXT PRIMARY KEY,"
                                "account_id TEXT REFERENCES accounts(account_id) ON DELETE CASCADE,"
                                "kind TEXT NOT NULL,"
                                "payload_json TEXT NOT NULL,"
                                "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                                "read_at TEXT"
                                ") STRICT"),
                            QStringLiteral("CREATE TABLE IF NOT EXISTS settings ("
                                           "scope TEXT NOT NULL,"
                                           "key TEXT NOT NULL,"
                                           "value_json TEXT NOT NULL,"
                                           "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                                           "PRIMARY KEY (scope, key)"
                                           ") STRICT"),
                            QStringLiteral(
                                "CREATE INDEX IF NOT EXISTS idx_mailboxes_parent ON mailboxes "
                                "(account_id, parent_mailbox_id, sort_order, mailbox_id)"),
                            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_emails_thread ON emails "
                                           "(account_id, "
                                           "thread_id)"),
                            QStringLiteral(
                                "CREATE INDEX IF NOT EXISTS idx_emails_received ON emails "
                                "(account_id, received_at DESC, email_id)"),
                            QStringLiteral(
                                "CREATE INDEX IF NOT EXISTS idx_email_mailboxes_mailbox ON "
                                "email_mailboxes (account_id, mailbox_id, email_id)"),
                            QStringLiteral(
                                "CREATE INDEX IF NOT EXISTS idx_email_keywords_keyword ON "
                                "email_keywords (account_id, keyword, email_id)"),
                            QStringLiteral(
                                "CREATE INDEX IF NOT EXISTS idx_sync_state_object ON sync_state "
                                "(account_id, object_type, query_key)"),
                            QStringLiteral(
                                "CREATE INDEX IF NOT EXISTS idx_pending_actions_status ON "
                                "pending_actions (account_id, status, created_at)"),
                        },
                },
                MigrationStep{
                    .version = 2,
                    .name = QStringLiteral("mailboxes_is_subscribed"),
                    .statements =
                        {
                            QStringLiteral(
                                "ALTER TABLE mailboxes ADD COLUMN is_subscribed INTEGER NOT NULL "
                                "DEFAULT 0"),
                        },
                },
                MigrationStep{
                    .version = 3,
                    .name = QStringLiteral("session_and_account_metadata"),
                    .statements =
                        {
                            QStringLiteral(
                                "ALTER TABLE accounts ADD COLUMN name TEXT NOT NULL DEFAULT ''"),
                            QStringLiteral("ALTER TABLE accounts ADD COLUMN is_personal INTEGER "
                                           "NOT NULL DEFAULT "
                                           "0"),
                            QStringLiteral("ALTER TABLE accounts ADD COLUMN is_read_only INTEGER "
                                           "NOT NULL DEFAULT "
                                           "0"),
                            QStringLiteral("ALTER TABLE accounts ADD COLUMN cap_mail INTEGER NOT "
                                           "NULL DEFAULT 0"),
                            QStringLiteral(
                                "ALTER TABLE accounts ADD COLUMN cap_submission INTEGER NOT NULL "
                                "DEFAULT 0"),
                            QStringLiteral("ALTER TABLE sessions ADD COLUMN has_core_capability "
                                           "INTEGER NOT NULL "
                                           "DEFAULT 0"),
                            QStringLiteral("ALTER TABLE sessions ADD COLUMN has_mail_capability "
                                           "INTEGER NOT NULL "
                                           "DEFAULT 0"),
                            QStringLiteral("ALTER TABLE sessions ADD COLUMN "
                                           "has_submission_capability INTEGER NOT "
                                           "NULL DEFAULT 0"),
                            QStringLiteral("ALTER TABLE sessions ADD COLUMN core_capabilities_json "
                                           "TEXT NOT NULL "
                                           "DEFAULT 'null'"),
                            QStringLiteral(
                                "ALTER TABLE sessions ADD COLUMN primary_mail_account_id TEXT"),
                            QStringLiteral("ALTER TABLE sessions ADD COLUMN "
                                           "primary_submission_account_id TEXT"),
                        },
                },
                MigrationStep{
                    .version = 4,
                    .name = QStringLiteral("compose_and_threading_metadata"),
                    .statements =
                        {
                            QStringLiteral("ALTER TABLE emails ADD COLUMN message_id_json TEXT "
                                           "NOT NULL DEFAULT '[]'"),
                            QStringLiteral("ALTER TABLE emails ADD COLUMN in_reply_to_json TEXT "
                                           "NOT NULL DEFAULT '[]'"),
                            QStringLiteral("ALTER TABLE emails ADD COLUMN references_json TEXT "
                                           "NOT NULL DEFAULT '[]'"),
                            QStringLiteral("CREATE TABLE IF NOT EXISTS compose_sessions ("
                                           "compose_session_id TEXT PRIMARY KEY,"
                                           "account_id TEXT NOT NULL REFERENCES "
                                           "accounts(account_id) ON DELETE CASCADE,"
                                           "draft_email_id TEXT,"
                                           "mode TEXT NOT NULL,"
                                           "editor_mode TEXT NOT NULL,"
                                           "snapshot_json TEXT NOT NULL,"
                                           "last_saved_at TEXT,"
                                           "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
                                           ") STRICT"),
                            QStringLiteral(
                                "CREATE INDEX IF NOT EXISTS idx_compose_sessions_account ON "
                                "compose_sessions (account_id, updated_at DESC)"),
                        },
                },
                MigrationStep{
                    .version = 5,
                    .name = QStringLiteral("raw_message_sources"),
                    .statements =
                        {
                            QStringLiteral("CREATE TABLE IF NOT EXISTS raw_message_sources ("
                                           "account_id TEXT NOT NULL REFERENCES "
                                           "accounts(account_id) ON DELETE CASCADE,"
                                           "email_id TEXT NOT NULL,"
                                           "blob_id TEXT NOT NULL,"
                                           "payload BLOB NOT NULL,"
                                           "fetched_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                                           "PRIMARY KEY (account_id, email_id)"
                                           ") STRICT"),
                            QStringLiteral(
                                "CREATE INDEX IF NOT EXISTS idx_raw_message_sources_blob ON "
                                "raw_message_sources (account_id, blob_id)"),
                        },
                },
                MigrationStep{
                    .version = 8,
                    .name = QStringLiteral("account_session_ownership"),
                    .statements =
                        {
                            QStringLiteral("ALTER TABLE accounts ADD COLUMN owner_account_id TEXT"),
                            QStringLiteral("UPDATE accounts SET owner_account_id = account_id"),
                            QStringLiteral(
                                "CREATE INDEX IF NOT EXISTS idx_accounts_owner ON accounts "
                                "(owner_account_id, account_id)"),
                        },
                },
                MigrationStep{
                    .version = 9,
                    .name = QStringLiteral("ensure_raw_message_sources"),
                    .statements =
                        {
                            QStringLiteral("CREATE TABLE IF NOT EXISTS raw_message_sources ("
                                           "account_id TEXT NOT NULL REFERENCES "
                                           "accounts(account_id) ON DELETE CASCADE,"
                                           "email_id TEXT NOT NULL,"
                                           "blob_id TEXT NOT NULL,"
                                           "payload BLOB NOT NULL,"
                                           "fetched_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                                           "PRIMARY KEY (account_id, email_id)"
                                           ") STRICT"),
                            QStringLiteral(
                                "CREATE INDEX IF NOT EXISTS idx_raw_message_sources_blob ON "
                                "raw_message_sources (account_id, blob_id)"),
                        },
                },
                MigrationStep{
                    .version = 10,
                    .name = QStringLiteral("translation_cache"),
                    .statements =
                        {
                            QStringLiteral("CREATE TABLE IF NOT EXISTS translation_cache ("
                                           "source_language TEXT NOT NULL,"
                                           "target_language TEXT NOT NULL,"
                                           "input_hash TEXT NOT NULL,"
                                           "input_text TEXT NOT NULL,"
                                           "translated_text TEXT NOT NULL,"
                                           "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                                           "PRIMARY KEY (source_language, target_language, "
                                           "input_hash)"
                                           ") STRICT"),
                            QStringLiteral(
                                "CREATE INDEX IF NOT EXISTS idx_translation_cache_updated ON "
                                "translation_cache (updated_at DESC)"),
                        },
                },
                MigrationStep{
                    .version = 11,
                    .name = QStringLiteral("contacts_capabilities"),
                    .statements =
                        {
                            QStringLiteral("ALTER TABLE accounts ADD COLUMN cap_contacts INTEGER "
                                           "NOT NULL DEFAULT 0"),
                            QStringLiteral("ALTER TABLE accounts ADD COLUMN "
                                           "contacts_capabilities_json TEXT NOT NULL DEFAULT "
                                           "'null'"),
                            QStringLiteral("ALTER TABLE sessions ADD COLUMN "
                                           "has_contacts_capability INTEGER NOT NULL DEFAULT 0"),
                            QStringLiteral("ALTER TABLE sessions ADD COLUMN "
                                           "primary_contacts_account_id TEXT"),
                        },
                },
                MigrationStep{
                    .version = 12,
                    .name = QStringLiteral("contacts_cache"),
                    .statements =
                        {
                            QStringLiteral(
                                "CREATE TABLE address_books (account_id TEXT NOT NULL "
                                "REFERENCES accounts(account_id) ON DELETE CASCADE, "
                                "address_book_id TEXT NOT NULL, name TEXT NOT NULL, "
                                "description TEXT, sort_order INTEGER NOT NULL, "
                                "is_default INTEGER NOT NULL, is_subscribed INTEGER NOT "
                                "NULL, share_with_json TEXT NOT NULL, my_rights_json TEXT "
                                "NOT NULL, state TEXT NOT NULL, "
                                "PRIMARY KEY(account_id,address_book_id)) STRICT"),
                            QStringLiteral(
                                "CREATE TABLE contact_cards (account_id TEXT NOT NULL "
                                "REFERENCES accounts(account_id) ON DELETE CASCADE, "
                                "contact_id TEXT NOT NULL, uid TEXT NOT NULL, kind TEXT "
                                "NOT NULL, display_name TEXT NOT NULL, organization TEXT, "
                                "document_json TEXT NOT NULL, PRIMARY KEY(account_id,"
                                "contact_id), UNIQUE(account_id,uid)) STRICT"),
                            QStringLiteral(
                                "CREATE TABLE contact_card_address_books (account_id TEXT "
                                "NOT NULL, contact_id TEXT NOT NULL, address_book_id TEXT "
                                "NOT NULL, PRIMARY KEY(account_id,contact_id,address_book_id), "
                                "FOREIGN KEY(account_id,contact_id) REFERENCES contact_cards"
                                "(account_id,contact_id) ON DELETE CASCADE, FOREIGN KEY"
                                "(account_id,address_book_id) REFERENCES address_books"
                                "(account_id,address_book_id) ON DELETE CASCADE) STRICT"),
                            QStringLiteral(
                                "CREATE TABLE contact_emails (account_id TEXT NOT NULL, "
                                "contact_id TEXT NOT NULL, entry_key TEXT NOT NULL, address "
                                "TEXT NOT NULL, normalized_address TEXT NOT NULL, label TEXT, "
                                "preference INTEGER, PRIMARY KEY(account_id,contact_id,entry_key), "
                                "FOREIGN KEY(account_id,contact_id) REFERENCES contact_cards"
                                "(account_id,contact_id) ON DELETE CASCADE) STRICT"),
                            QStringLiteral("CREATE INDEX idx_contact_cards_name ON contact_cards"
                                           "(account_id,display_name COLLATE NOCASE,contact_id)"),
                            QStringLiteral(
                                "CREATE INDEX idx_contact_emails_address ON contact_emails"
                                "(normalized_address,account_id,contact_id)"),
                            QStringLiteral("CREATE INDEX idx_contact_books_membership ON "
                                           "contact_card_address_books(account_id,address_book_id,"
                                           "contact_id)"),
                        },
                },
                MigrationStep{
                    .version = 13,
                    .name = QStringLiteral("websocket_push_capability"),
                    .statements =
                        {
                            QStringLiteral("ALTER TABLE sessions ADD COLUMN websocket_url TEXT"),
                            QStringLiteral("ALTER TABLE sessions ADD COLUMN "
                                           "websocket_supports_push INTEGER NOT NULL DEFAULT 0"),
                        },
                },
                MigrationStep{
                    .version = 14,
                    .name = QStringLiteral("search_windows"),
                    .statements =
                        {
                            QStringLiteral(
                                "CREATE TABLE search_windows (account_id TEXT NOT NULL REFERENCES "
                                "accounts(account_id) ON DELETE CASCADE, query_key TEXT NOT NULL, "
                                "window_offset INTEGER NOT NULL, window_limit INTEGER NOT NULL, "
                                "total INTEGER, updated_at TEXT NOT NULL DEFAULT "
                                "CURRENT_TIMESTAMP, "
                                "PRIMARY KEY(account_id,query_key,window_offset,window_limit)) "
                                "STRICT"),
                            QStringLiteral(
                                "CREATE TABLE search_window_items (account_id TEXT NOT NULL, "
                                "query_key TEXT NOT NULL, window_offset INTEGER NOT NULL, "
                                "window_limit INTEGER NOT NULL, position INTEGER NOT NULL, "
                                "email_id TEXT NOT NULL, "
                                "PRIMARY KEY(account_id,query_key,window_offset,window_limit,"
                                "position), FOREIGN KEY(account_id,query_key,window_offset,"
                                "window_limit) REFERENCES search_windows(account_id,query_key,"
                                "window_offset,window_limit) ON DELETE CASCADE) STRICT"),
                            QStringLiteral(
                                "CREATE INDEX idx_search_window_items_email ON search_window_items"
                                "(account_id,email_id)"),
                        },
                },
                MigrationStep{
                    .version = 15,
                    .name = QStringLiteral("jmap_transport_preferences"),
                    .statements =
                        {
                            QStringLiteral(
                                "CREATE TABLE jmap_transport_preferences ("
                                "owner_account_id TEXT PRIMARY KEY REFERENCES accounts(account_id) "
                                "ON DELETE CASCADE, websocket_url TEXT NOT NULL, mode TEXT NOT "
                                "NULL "
                                "CHECK(mode IN ('unknown','websocket','http_fallback')), "
                                "retry_after TEXT, last_error TEXT, updated_at TEXT NOT NULL "
                                "DEFAULT "
                                "CURRENT_TIMESTAMP) STRICT"),
                        },
                },
                MigrationStep{
                    .version = 16,
                    .name = QStringLiteral("observed_notification_emails"),
                    .statements =
                        {
                            QStringLiteral(
                                "CREATE TABLE observed_notification_emails ("
                                "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON "
                                "DELETE CASCADE, email_id TEXT NOT NULL, observed_at TEXT NOT NULL "
                                "DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY(account_id,email_id)) "
                                "STRICT"),
                            QStringLiteral(
                                "INSERT INTO observed_notification_emails (account_id,email_id) "
                                "SELECT account_id,email_id FROM emails"),
                        },
                },
                MigrationStep{
                    .version = 17,
                    .name = QStringLiteral("calendar_cache"),
                    .statements =
                        {
                            QStringLiteral("ALTER TABLE accounts ADD COLUMN cap_calendars INTEGER "
                                           "NOT NULL DEFAULT 0"),
                            QStringLiteral("ALTER TABLE accounts ADD COLUMN "
                                           "calendars_capabilities_json TEXT NOT NULL DEFAULT "
                                           "'null'"),
                            QStringLiteral("ALTER TABLE sessions ADD COLUMN "
                                           "has_calendars_capability INTEGER NOT NULL DEFAULT 0"),
                            QStringLiteral("ALTER TABLE sessions ADD COLUMN "
                                           "primary_calendars_account_id TEXT"),
                            QStringLiteral(
                                "CREATE TABLE calendars (account_id TEXT NOT NULL REFERENCES "
                                "accounts(account_id) ON DELETE CASCADE, calendar_id TEXT NOT "
                                "NULL, name TEXT NOT NULL, description TEXT, color TEXT, "
                                "sort_order INTEGER NOT NULL DEFAULT 0, is_subscribed INTEGER NOT "
                                "NULL, is_visible INTEGER NOT NULL, is_default INTEGER NOT NULL, "
                                "time_zone TEXT, rights_json TEXT NOT NULL, state TEXT, PRIMARY "
                                "KEY(account_id,calendar_id)) STRICT"),
                            QStringLiteral(
                                "CREATE TABLE calendar_events (account_id TEXT NOT NULL "
                                "REFERENCES accounts(account_id) ON DELETE CASCADE, event_id TEXT "
                                "NOT NULL, uid TEXT NOT NULL, title TEXT NOT NULL DEFAULT '', "
                                "description TEXT, location TEXT, document_json TEXT NOT NULL, "
                                "state TEXT, PRIMARY KEY(account_id,event_id)) STRICT"),
                            QStringLiteral(
                                "CREATE TABLE calendar_event_calendars (account_id TEXT NOT NULL, "
                                "event_id TEXT NOT NULL, calendar_id TEXT NOT NULL, PRIMARY KEY"
                                "(account_id,event_id,calendar_id), FOREIGN "
                                "KEY(account_id,event_id) "
                                "REFERENCES calendar_events(account_id,event_id) ON DELETE "
                                "CASCADE, "
                                "FOREIGN KEY(account_id,calendar_id) REFERENCES calendars"
                                "(account_id,calendar_id) ON DELETE CASCADE) STRICT"),
                            QStringLiteral(
                                "CREATE TABLE calendar_occurrences (account_id TEXT NOT NULL, "
                                "occurrence_id TEXT NOT NULL, event_id TEXT NOT NULL, "
                                "recurrence_id "
                                "TEXT, start_utc TEXT, end_utc TEXT, local_start TEXT NOT NULL, "
                                "local_end TEXT NOT NULL, is_all_day INTEGER NOT NULL, PRIMARY KEY"
                                "(account_id,occurrence_id), FOREIGN KEY(account_id,event_id) "
                                "REFERENCES calendar_events(account_id,event_id) ON DELETE "
                                "CASCADE) "
                                "STRICT"),
                            QStringLiteral(
                                "CREATE TABLE calendar_query_windows (account_id TEXT NOT NULL "
                                "REFERENCES accounts(account_id) ON DELETE CASCADE, range_start "
                                "TEXT NOT NULL, range_end TEXT NOT NULL, display_time_zone TEXT "
                                "NOT "
                                "NULL, query_state TEXT NOT NULL, updated_at TEXT NOT NULL DEFAULT "
                                "CURRENT_TIMESTAMP, PRIMARY KEY(account_id,range_start,range_end,"
                                "display_time_zone)) STRICT"),
                            QStringLiteral(
                                "CREATE TABLE calendar_window_occurrences (account_id TEXT NOT "
                                "NULL, range_start TEXT NOT NULL, range_end TEXT NOT NULL, "
                                "display_time_zone TEXT NOT NULL, occurrence_id TEXT NOT NULL, "
                                "PRIMARY KEY(account_id,range_start,range_end,display_time_zone,"
                                "occurrence_id), FOREIGN KEY(account_id,range_start,range_end,"
                                "display_time_zone) REFERENCES calendar_query_windows(account_id,"
                                "range_start,range_end,display_time_zone) ON DELETE CASCADE, "
                                "FOREIGN "
                                "KEY(account_id,occurrence_id) REFERENCES calendar_occurrences"
                                "(account_id,occurrence_id) ON DELETE CASCADE) STRICT"),
                            QStringLiteral(
                                "CREATE TABLE calendar_state_tokens (account_id TEXT NOT NULL "
                                "REFERENCES accounts(account_id) ON DELETE CASCADE, data_type TEXT "
                                "NOT NULL CHECK(data_type IN ('Calendar','CalendarEvent')), state "
                                "TEXT NOT NULL, PRIMARY KEY(account_id,data_type)) STRICT"),
                            QStringLiteral(
                                "CREATE INDEX idx_calendar_occurrences_range ON "
                                "calendar_occurrences(account_id,local_start,local_end)"),
                            QStringLiteral(
                                "CREATE INDEX idx_calendar_membership_calendar ON "
                                "calendar_event_calendars(account_id,calendar_id,event_id)"),
                        },
                },
                MigrationStep{
                    .version = 18,
                    .name = QStringLiteral("calendar_preferences"),
                    .statements =
                        {
                            QStringLiteral(
                                "CREATE TABLE calendar_preferences (account_id TEXT NOT NULL, "
                                "calendar_id TEXT NOT NULL, is_visible INTEGER NOT NULL, "
                                "is_default_destination INTEGER NOT NULL DEFAULT 0, PRIMARY KEY"
                                "(account_id,calendar_id), FOREIGN KEY(account_id,calendar_id) "
                                "REFERENCES calendars(account_id,calendar_id) ON DELETE CASCADE) "
                                "STRICT"),
                            QStringLiteral(
                                "CREATE UNIQUE INDEX idx_calendar_default_destination ON "
                                "calendar_preferences(account_id) WHERE "
                                "is_default_destination=1"),
                        },
                },
                MigrationStep{
                    .version = 19,
                    .name = QStringLiteral("email_full_text_search"),
                    .statements =
                        {
                            QStringLiteral("CREATE VIRTUAL TABLE email_search_fts USING fts5("
                                           "account_id UNINDEXED,email_id UNINDEXED,subject,body,"
                                           "body_blob_id UNINDEXED,"
                                           "tokenize='unicode61')"),
                            QStringLiteral(
                                "INSERT INTO email_search_fts(account_id,email_id,subject,body,"
                                "body_blob_id) SELECT "
                                "account_id,email_id,COALESCE(subject,''),'','' "
                                "FROM emails"),
                            QStringLiteral(
                                "CREATE TRIGGER emails_search_insert AFTER INSERT ON emails BEGIN "
                                "INSERT INTO email_search_fts(account_id,email_id,subject,body,"
                                "body_blob_id) VALUES(new.account_id,new.email_id,"
                                "COALESCE(new.subject,''),'',''); "
                                "END"),
                            QStringLiteral(
                                "CREATE TRIGGER emails_search_subject_update AFTER UPDATE OF "
                                "subject "
                                "ON emails BEGIN UPDATE email_search_fts SET "
                                "subject=COALESCE(new.subject,'') WHERE account_id=old.account_id "
                                "AND email_id=old.email_id; END"),
                            QStringLiteral(
                                "CREATE TRIGGER emails_search_delete AFTER DELETE ON emails BEGIN "
                                "DELETE FROM email_search_fts WHERE account_id=old.account_id AND "
                                "email_id=old.email_id; END"),
                        },
                },
                MigrationStep{
                    .version = 20,
                    .name = QStringLiteral("consistency_domains"),
                    .statements =
                        {
                            QStringLiteral(
                                "CREATE TABLE consistency_domains ("
                                "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON "
                                "DELETE CASCADE,data_type TEXT NOT NULL,"
                                "mutation_generation INTEGER NOT NULL DEFAULT 0 "
                                "CHECK(mutation_generation>=0),"
                                "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                                "PRIMARY KEY(account_id,data_type)) STRICT"),
                        },
                },
                MigrationStep{
                    .version = 21,
                    .name = QStringLiteral("generic_mutation_journal"),
                    .statements =
                        {
                            QStringLiteral(
                                "CREATE TABLE mutation_journal ("
                                "mutation_id TEXT PRIMARY KEY,operation_group_id TEXT,"
                                "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON "
                                "DELETE CASCADE,data_type TEXT NOT NULL,object_id TEXT NOT NULL,"
                                "mutation_kind TEXT NOT NULL,status TEXT NOT NULL "
                                "CHECK(status IN ('pending','in_flight','accepted','rejected',"
                                "'unknown')),payload_json TEXT NOT NULL,base_state TEXT,"
                                "accepted_state TEXT,error_json TEXT,"
                                "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                                "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP) STRICT"),
                            QStringLiteral(
                                "INSERT INTO mutation_journal (mutation_id,account_id,data_type,"
                                "object_id,mutation_kind,status,payload_json,created_at,updated_at)"
                                " "
                                "SELECT pending_action_id,account_id,'Email',"
                                "json_extract(payload_json,'$.emailId'),'email_patch',"
                                "CASE status WHEN 'failed' THEN 'rejected' ELSE status END,"
                                "payload_json,created_at,updated_at FROM pending_actions"),
                            QStringLiteral("DROP TABLE pending_actions"),
                            QStringLiteral(
                                "CREATE INDEX idx_mutation_journal_status ON mutation_journal "
                                "(account_id,data_type,status,created_at)"),
                            QStringLiteral(
                                "CREATE INDEX idx_mutation_journal_object ON mutation_journal "
                                "(account_id,data_type,object_id,created_at)"),
                            QStringLiteral(
                                "CREATE INDEX idx_mutation_journal_group ON mutation_journal "
                                "(operation_group_id) WHERE operation_group_id IS NOT NULL"),
                        },
                },
                MigrationStep{
                    .version = 22,
                    .name = QStringLiteral("sieve_script_cache"),
                    .statements =
                        {
                            QStringLiteral(
                                "CREATE TABLE sieve_scripts ("
                                "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON "
                                "DELETE CASCADE,script_id TEXT NOT NULL,name TEXT NOT NULL,"
                                "blob_id TEXT NOT NULL,is_active INTEGER NOT NULL DEFAULT 0 "
                                "CHECK(is_active IN (0,1)),"
                                "PRIMARY KEY(account_id,script_id)) STRICT"),
                            QStringLiteral("CREATE INDEX idx_sieve_scripts_name ON sieve_scripts "
                                           "(account_id,name COLLATE NOCASE)"),
                        },
                },
                MigrationStep{
                    .version = 23,
                    .name = QStringLiteral("server_calendar_default"),
                    .statements =
                        {
                            QStringLiteral("DROP INDEX idx_calendar_default_destination"),
                            QStringLiteral("ALTER TABLE calendar_preferences DROP COLUMN "
                                           "is_default_destination"),
                        },
                },
                MigrationStep{
                    .version = 24,
                    .name = QStringLiteral("mailbox_query_windows"),
                    .statements =
                        {
                            QStringLiteral(
                                "CREATE TABLE mailbox_query_windows ("
                                "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON "
                                "DELETE CASCADE,mailbox_id TEXT NOT NULL,query_key TEXT NOT NULL,"
                                "requested_offset INTEGER NOT NULL,requested_limit INTEGER NOT "
                                "NULL,position INTEGER NOT NULL,returned_limit INTEGER NOT NULL,"
                                "total INTEGER,query_state TEXT NOT NULL,updated_at TEXT NOT NULL "
                                "DEFAULT CURRENT_TIMESTAMP,PRIMARY KEY(account_id,query_key,"
                                "requested_offset,requested_limit)) STRICT"),
                            QStringLiteral(
                                "CREATE TABLE mailbox_query_window_items (account_id TEXT NOT "
                                "NULL,query_key TEXT NOT NULL,requested_offset INTEGER NOT NULL,"
                                "requested_limit INTEGER NOT NULL,position INTEGER NOT NULL,"
                                "email_id TEXT NOT NULL,PRIMARY KEY(account_id,query_key,"
                                "requested_offset,requested_limit,position),FOREIGN KEY(account_id,"
                                "query_key,requested_offset,requested_limit) REFERENCES "
                                "mailbox_query_windows(account_id,query_key,requested_offset,"
                                "requested_limit) ON DELETE CASCADE) STRICT"),
                            QStringLiteral(
                                "CREATE INDEX idx_mailbox_query_windows_mailbox ON "
                                "mailbox_query_windows(account_id,mailbox_id,updated_at)"),
                            QStringLiteral(
                                "ALTER TABLE search_windows ADD COLUMN position INTEGER NOT NULL "
                                "DEFAULT 0"),
                            QStringLiteral(
                                "ALTER TABLE search_windows ADD COLUMN returned_limit INTEGER NOT "
                                "NULL DEFAULT 0"),
                            QStringLiteral(
                                "ALTER TABLE search_windows ADD COLUMN query_state TEXT NOT NULL "
                                "DEFAULT ''"),
                        },
                },
                MigrationStep{
                    .version = 25,
                    .name = QStringLiteral("calendar_notification_state"),
                    .statements =
                        {
                            QStringLiteral(
                                "CREATE TABLE calendar_notification_state (notification_key TEXT "
                                "PRIMARY KEY,status TEXT NOT NULL CHECK(status IN "
                                "('notified','dismissed','snoozed')),notified_at TEXT NOT NULL,"
                                "snoozed_until TEXT) STRICT"),
                            QStringLiteral("CREATE INDEX idx_calendar_notification_snooze ON "
                                           "calendar_notification_state(status,snoozed_until)"),
                        },
                },
                MigrationStep{
                    .version = 26,
                    .name = QStringLiteral("calendar_default_alerts"),
                    .statements =
                        {
                            QStringLiteral(
                                "CREATE TABLE calendar_default_alerts (account_id TEXT NOT NULL,"
                                "calendar_id TEXT NOT NULL,alert_id TEXT NOT NULL,without_time "
                                "INTEGER NOT NULL CHECK(without_time IN (0,1)),action TEXT NOT "
                                "NULL,trigger_kind TEXT NOT NULL CHECK(trigger_kind IN "
                                "('offset','absolute')),relative_to TEXT NOT NULL,offset TEXT,"
                                "trigger_at TEXT,acknowledged TEXT,PRIMARY KEY(account_id,"
                                "calendar_id,alert_id,without_time),FOREIGN KEY(account_id,"
                                "calendar_id) REFERENCES calendars(account_id,calendar_id) ON "
                                "DELETE CASCADE) STRICT"),
                        },
                },
                MigrationStep{
                    .version = 27,
                    .name = QStringLiteral("mailbox_query_window_validity"),
                    .statements =
                        {
                            QStringLiteral(
                                "ALTER TABLE mailbox_query_windows ADD COLUMN is_valid INTEGER "
                                "NOT NULL DEFAULT 1 CHECK(is_valid IN (0,1))"),
                        },
                },
                MigrationStep{
                    .version = 28,
                    .name = QStringLiteral("mail_notification_outbox"),
                    .statements =
                        {
                            QStringLiteral(
                                "CREATE TABLE mail_notification_outbox ("
                                "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON "
                                "DELETE CASCADE,mailbox_id TEXT NOT NULL,email_id TEXT NOT NULL,"
                                "thread_id TEXT NOT NULL,subject TEXT,received_at TEXT NOT NULL,"
                                "status TEXT NOT NULL CHECK(status IN ('pending','delivered')),"
                                "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,delivered_at "
                                "TEXT,PRIMARY KEY(account_id,email_id)) STRICT"),
                            QStringLiteral("CREATE INDEX idx_mail_notification_outbox_pending ON "
                                           "mail_notification_outbox(account_id,mailbox_id,status,"
                                           "received_at)"),
                        },
                },
                MigrationStep{
                    .version = 29,
                    .name = QStringLiteral("offline_mail_foundation"),
                    .statements =
                        {
                            QStringLiteral(
                                "CREATE TABLE offline_mailbox_scopes (account_id TEXT NOT NULL "
                                "REFERENCES accounts(account_id) ON DELETE CASCADE,mailbox_id "
                                "TEXT NOT NULL,desired INTEGER NOT NULL DEFAULT 1 CHECK(desired IN "
                                "(0,1)),status TEXT NOT NULL DEFAULT 'pending' CHECK(status IN "
                                "('pending','enumerating','fetching','reconciling','complete',"
                                "'paused','waiting_for_space','failed')),query_state TEXT,"
                                "email_state TEXT,anchor_email_id TEXT,expected_total INTEGER,"
                                "completed_total INTEGER NOT NULL DEFAULT 0,completed_bytes "
                                "INTEGER NOT NULL DEFAULT 0,estimated_bytes INTEGER,"
                                "generation INTEGER NOT NULL DEFAULT 0,completed_generation "
                                "INTEGER,latest_error TEXT,updated_at TEXT NOT NULL DEFAULT "
                                "CURRENT_TIMESTAMP,PRIMARY KEY(account_id,mailbox_id)) STRICT"),
                            QStringLiteral(
                                "CREATE TABLE offline_mailbox_membership (account_id TEXT NOT "
                                "NULL,mailbox_id TEXT NOT NULL,email_id TEXT NOT NULL,generation "
                                "INTEGER NOT NULL,position INTEGER NOT NULL,PRIMARY KEY(account_id,"
                                "mailbox_id,generation,email_id),FOREIGN "
                                "KEY(account_id,mailbox_id) "
                                "REFERENCES offline_mailbox_scopes(account_id,mailbox_id) ON "
                                "DELETE CASCADE) STRICT"),
                            QStringLiteral(
                                "CREATE UNIQUE INDEX idx_offline_membership_position ON "
                                "offline_mailbox_membership(account_id,mailbox_id,generation,"
                                "position)"),
                            QStringLiteral(
                                "CREATE TABLE mail_vault_objects (content_hash TEXT PRIMARY KEY,"
                                "relative_path TEXT NOT NULL UNIQUE,size INTEGER NOT NULL CHECK"
                                "(size>=0),created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP) "
                                "STRICT"),
                            QStringLiteral(
                                "CREATE TABLE mail_vault_email_refs (account_id TEXT NOT NULL "
                                "REFERENCES accounts(account_id) ON DELETE CASCADE,email_id TEXT "
                                "NOT NULL,blob_id TEXT NOT NULL,content_hash TEXT NOT NULL "
                                "REFERENCES "
                                "mail_vault_objects(content_hash),retention TEXT NOT NULL CHECK"
                                "(retention IN ('full_sync','evictable')),indexed_hash TEXT,"
                                "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,PRIMARY KEY"
                                "(account_id,email_id)) STRICT"),
                            QStringLiteral(
                                "CREATE INDEX idx_mail_vault_refs_hash ON mail_vault_email_refs"
                                "(content_hash)"),
                            QStringLiteral(
                                "CREATE TABLE mail_vault_projection_jobs (job_id INTEGER PRIMARY "
                                "KEY,account_id TEXT NOT NULL,email_id TEXT NOT NULL,mailbox_id "
                                "TEXT,content_hash TEXT,operation TEXT NOT NULL CHECK(operation IN "
                                "('link','unlink','metadata','gc')),status TEXT NOT NULL DEFAULT "
                                "'pending' CHECK(status IN ('pending','complete','failed')),"
                                "last_error TEXT,created_at TEXT NOT NULL DEFAULT "
                                "CURRENT_TIMESTAMP,"
                                "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP) STRICT"),
                            QStringLiteral("CREATE INDEX idx_mail_vault_projection_pending ON "
                                           "mail_vault_projection_jobs(status,job_id)"),
                            QStringLiteral(
                                "CREATE TABLE background_jobs (job_id TEXT PRIMARY "
                                "KEY,parent_job_id "
                                "TEXT REFERENCES background_jobs(job_id) ON DELETE "
                                "CASCADE,account_id "
                                "TEXT,kind TEXT NOT NULL,priority INTEGER NOT NULL,status TEXT NOT "
                                "NULL CHECK(status IN "
                                "('queued','running','paused','waiting_for_space',"
                                "'waiting_for_network','waiting_for_auth','failed','complete')),"
                                "title "
                                "TEXT NOT NULL,detail TEXT NOT NULL DEFAULT '',completed_units "
                                "INTEGER "
                                "NOT NULL DEFAULT 0,total_units INTEGER,completed_bytes INTEGER "
                                "NOT "
                                "NULL DEFAULT 0,total_bytes INTEGER,checkpoint_json TEXT NOT NULL "
                                "DEFAULT '{}',error_text TEXT,pause_requested INTEGER NOT NULL "
                                "DEFAULT "
                                "0 CHECK(pause_requested IN (0,1)),created_at TEXT NOT NULL "
                                "DEFAULT "
                                "CURRENT_TIMESTAMP,updated_at TEXT NOT NULL DEFAULT "
                                "CURRENT_TIMESTAMP) "
                                "STRICT"),
                            QStringLiteral(
                                "CREATE INDEX idx_background_jobs_dispatch ON background_jobs"
                                "(status,priority,created_at)"),
                            QStringLiteral(
                                "CREATE TABLE background_job_dependencies (job_id TEXT NOT NULL "
                                "REFERENCES background_jobs(job_id) ON DELETE "
                                "CASCADE,depends_on_job_id "
                                "TEXT NOT NULL REFERENCES background_jobs(job_id) ON DELETE "
                                "CASCADE,"
                                "PRIMARY KEY(job_id,depends_on_job_id)) STRICT"),
                            QStringLiteral("CREATE TABLE local_data_migrations (migration_key TEXT "
                                           "PRIMARY KEY,"
                                           "status TEXT NOT NULL CHECK(status IN "
                                           "('pending','running','complete',"
                                           "'failed')),checkpoint TEXT,latest_error "
                                           "TEXT,updated_at TEXT NOT NULL "
                                           "DEFAULT CURRENT_TIMESTAMP) STRICT"),
                            QStringLiteral(
                                "INSERT INTO local_data_migrations(migration_key,status) VALUES"
                                "('raw_message_sources_to_vault','pending')"),
                        },
                },
                MigrationStep{
                    .version = 30,
                    .name = QStringLiteral("external_rebuildable_mail_search"),
                    .statements =
                        {
                            QStringLiteral("DROP TRIGGER emails_search_insert"),
                            QStringLiteral("DROP TRIGGER emails_search_subject_update"),
                            QStringLiteral("DROP TRIGGER emails_search_delete"),
                            QStringLiteral("DROP TABLE email_search_fts"),
                        },
                },
                MigrationStep{
                    .version = 31,
                    .name = QStringLiteral("retained_stale_search_windows"),
                    .statements =
                        {
                            QStringLiteral("ALTER TABLE search_windows ADD COLUMN is_valid "
                                           "INTEGER NOT NULL DEFAULT 1 CHECK(is_valid IN (0,1))"),
                        },
                },
                MigrationStep{
                    .version = 32,
                    .name = QStringLiteral("operation_history"),
                    .statements =
                        {
                            QStringLiteral(
                                "CREATE TABLE operation_history ("
                                "entry_id TEXT PRIMARY KEY,"
                                "stack TEXT NOT NULL CHECK(stack IN ('undo','redo')),"
                                "stack_order INTEGER NOT NULL,"
                                "domain TEXT NOT NULL,"
                                "command_kind TEXT NOT NULL,"
                                "label TEXT NOT NULL,"
                                "payload_version INTEGER NOT NULL,"
                                "payload_json TEXT NOT NULL,"
                                "status TEXT NOT NULL CHECK(status IN "
                                "('preparing','executing_forward','ready','executing_undo',"
                                "'executing_redo','blocked_unknown','blocked_partial',"
                                "'impossible','expired')),"
                                "operation_group_id TEXT,"
                                "expires_at TEXT,"
                                "explanation TEXT,"
                                "failure_json TEXT,"
                                "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                                "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
                                ") STRICT"),
                            QStringLiteral("CREATE UNIQUE INDEX idx_operation_history_stack_order "
                                           "ON operation_history(stack,stack_order)"),
                            QStringLiteral("CREATE INDEX idx_operation_history_operation_group "
                                           "ON operation_history(operation_group_id)"),
                            QStringLiteral("CREATE TABLE operation_history_sequence ("
                                           "singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
                                           "next_value INTEGER NOT NULL"
                                           ") STRICT"),
                            QStringLiteral(
                                "INSERT INTO operation_history_sequence(singleton,next_value) "
                                "VALUES(1,1)"),
                        },
                },
                MigrationStep{
                    .version = 33,
                    .name = QStringLiteral("deferred_sends"),
                    .statements =
                        {
                            QStringLiteral(
                                "CREATE TABLE pending_sends ("
                                "send_id TEXT PRIMARY KEY,"
                                "history_entry_id TEXT NOT NULL UNIQUE,"
                                "connection_id TEXT NOT NULL,"
                                "account_id TEXT NOT NULL,"
                                "compose_session_id TEXT NOT NULL,"
                                "draft_email_id TEXT NOT NULL,"
                                "subject TEXT,"
                                "status TEXT NOT NULL CHECK(status IN "
                                "('scheduled','waiting_for_network','waiting_for_auth',"
                                "'dispatching','submitted','cancelled','failed','unknown')),"
                                "due_at TEXT NOT NULL,"
                                "dispatch_started_at TEXT,"
                                "submission_id TEXT,"
                                "last_error TEXT,"
                                "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                                "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                                "FOREIGN KEY(history_entry_id) REFERENCES "
                                "operation_history(entry_id) ON DELETE CASCADE"
                                ") STRICT"),
                            QStringLiteral("CREATE INDEX idx_pending_sends_status_due "
                                           "ON pending_sends(status,due_at)"),
                            QStringLiteral("CREATE INDEX idx_pending_sends_compose_session "
                                           "ON pending_sends(compose_session_id)"),
                        },
                },
                MigrationStep{
                    .version = 34,
                    .name = QStringLiteral("typed_query_window_coverage"),
                    .statements =
                        {
                            QStringLiteral(
                                "ALTER TABLE mailbox_query_windows ADD COLUMN coverage TEXT NOT "
                                "NULL DEFAULT 'server' CHECK(coverage IN "
                                "('server','locally_projected','stale'))"),
                            QStringLiteral(
                                "UPDATE mailbox_query_windows SET coverage=CASE is_valid WHEN 1 "
                                "THEN 'server' ELSE 'stale' END"),
                            QStringLiteral(
                                "ALTER TABLE mailbox_query_windows DROP COLUMN is_valid"),
                            QStringLiteral(
                                "ALTER TABLE search_windows ADD COLUMN coverage TEXT NOT NULL "
                                "DEFAULT 'server' CHECK(coverage IN "
                                "('server','locally_projected','stale'))"),
                            QStringLiteral(
                                "UPDATE search_windows SET coverage=CASE is_valid WHEN 1 THEN "
                                "'server' ELSE 'stale' END"),
                            QStringLiteral("ALTER TABLE search_windows DROP COLUMN is_valid"),
                        },
                },
                MigrationStep{
                    .version = 35,
                    .name = QStringLiteral("calendar_deletion_projections"),
                    .statements =
                        {
                            QStringLiteral("CREATE TABLE calendar_deletion_projections ("
                                           "account_id TEXT NOT NULL,"
                                           "calendar_id TEXT NOT NULL,"
                                           "mutation_id TEXT NOT NULL UNIQUE,"
                                           "PRIMARY KEY(account_id,calendar_id),"
                                           "FOREIGN KEY(account_id,calendar_id) REFERENCES "
                                           "calendars(account_id,calendar_id) ON DELETE CASCADE"
                                           ") STRICT"),
                        },
                },
                MigrationStep{
                    .version = 36,
                    .name = QStringLiteral("query_window_materialization"),
                    .statements =
                        {
                            QStringLiteral(
                                "ALTER TABLE mailbox_query_windows ADD COLUMN materialization "
                                "TEXT NOT NULL DEFAULT 'complete' CHECK(materialization IN "
                                "('complete','partial'))"),
                            QStringLiteral(
                                "UPDATE mailbox_query_windows SET materialization='partial' "
                                "WHERE coverage!='server'"),
                            QStringLiteral(
                                "ALTER TABLE search_windows ADD COLUMN materialization TEXT NOT "
                                "NULL DEFAULT 'complete' CHECK(materialization IN "
                                "('complete','partial'))"),
                            QStringLiteral("UPDATE search_windows SET materialization='partial' "
                                           "WHERE coverage!='server'"),
                        },
                },
                MigrationStep{
                    .version = 37,
                    .name = QStringLiteral("ordered_mutation_journal"),
                    .statements =
                        {
                            QStringLiteral("ALTER TABLE mutation_journal ADD COLUMN sequence "
                                           "INTEGER NOT NULL DEFAULT 0"),
                            QStringLiteral(
                                "WITH ordered AS (SELECT mutation_id,ROW_NUMBER() OVER "
                                "(ORDER BY created_at,mutation_id) AS value FROM mutation_journal) "
                                "UPDATE mutation_journal SET sequence=(SELECT value FROM ordered "
                                "WHERE ordered.mutation_id=mutation_journal.mutation_id)"),
                            QStringLiteral("CREATE UNIQUE INDEX idx_mutation_journal_sequence ON "
                                           "mutation_journal(sequence)"),
                            QStringLiteral(
                                "CREATE TABLE mutation_journal_sequence ("
                                "singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
                                "next_value INTEGER NOT NULL CHECK(next_value>0)) STRICT"),
                            QStringLiteral(
                                "INSERT INTO mutation_journal_sequence(singleton,next_value) "
                                "SELECT 1,COALESCE(MAX(sequence),0)+1 FROM mutation_journal"),
                        },
                },
                MigrationStep{
                    .version = 38,
                    .name = QStringLiteral("notification_dispatch_claims"),
                    .statements =
                        {
                            QStringLiteral(
                                "CREATE TABLE notification_dispatch_claims ("
                                "kind TEXT NOT NULL CHECK(kind IN ('mail','calendar')),"
                                "claim_key TEXT NOT NULL,claimed_at TEXT NOT NULL DEFAULT "
                                "CURRENT_TIMESTAMP,PRIMARY KEY(kind,claim_key)) STRICT"),
                        },
                },
                MigrationStep{
                    .version = 39,
                    .name = QStringLiteral("drop_daemon_translation_cache"),
                    .statements =
                        {
                            QStringLiteral("DROP TABLE IF EXISTS translation_cache"),
                        },
                },
                MigrationStep{
                    .version = 40,
                    .name = QStringLiteral("mail_vault_mailbox_refs"),
                    .statements =
                        {
                            QStringLiteral(
                                "CREATE TABLE mail_vault_mailbox_refs (account_id TEXT NOT NULL,"
                                "email_id TEXT NOT NULL,mailbox_id TEXT NOT NULL,PRIMARY KEY("
                                "account_id,email_id,mailbox_id),FOREIGN KEY(account_id,email_id) "
                                "REFERENCES mail_vault_email_refs(account_id,email_id) ON DELETE "
                                "CASCADE) STRICT"),
                            QStringLiteral(
                                "CREATE INDEX idx_mail_vault_mailbox_refs_mailbox ON "
                                "mail_vault_mailbox_refs(account_id,mailbox_id,email_id)"),
                            QStringLiteral(
                                "INSERT INTO "
                                "mail_vault_mailbox_refs(account_id,email_id,mailbox_id) "
                                "SELECT em.account_id,em.email_id,em.mailbox_id FROM "
                                "email_mailboxes em JOIN mail_vault_email_refs r ON "
                                "r.account_id=em.account_id AND r.email_id=em.email_id"),
                        },
                },
                MigrationStep{
                    .version = 41,
                    .name = QStringLiteral("identity_create_projections"),
                    .statements =
                        {
                            QStringLiteral(
                                "CREATE TABLE identity_create_projections ("
                                "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON "
                                "DELETE CASCADE,creation_id TEXT NOT NULL,mutation_id TEXT NOT "
                                "NULL "
                                "UNIQUE,email_address TEXT NOT NULL,name TEXT NOT NULL DEFAULT '',"
                                "reply_to_json TEXT NOT NULL DEFAULT '[]',bcc_json TEXT NOT NULL "
                                "DEFAULT '[]',text_signature TEXT,html_signature TEXT,PRIMARY KEY("
                                "account_id,creation_id),FOREIGN KEY(mutation_id) REFERENCES "
                                "mutation_journal(mutation_id) ON DELETE CASCADE) STRICT"),
                            QStringLiteral("CREATE INDEX idx_identity_create_projection_account ON "
                                           "identity_create_projections(account_id,creation_id)"),
                        },
                },
            },
        };
    }

} // namespace javelin::jmap::cache
