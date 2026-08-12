#include "storage/sqlite/DatabaseConnection.h"

#include "storage/migrations/MigrationRunner.h"

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

        [[nodiscard]] std::optional<DatabaseError>
        executeStatement(QSqlDatabase& database, const QString& statement, const QString& operation)
        {
            QSqlQuery query{database};
            if (!query.exec(statement))
                return makeQueryError(DatabaseErrorCode::QueryFailed, operation, query);
            return std::nullopt;
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
                    return error;
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
            return *this;

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
            return *error;

        DatabaseWriteScope writeScope{connection};
        QSqlQuery begin{connection.database()};
        if (!begin.exec(QStringLiteral("BEGIN IMMEDIATE TRANSACTION")))
            return makeQueryError(DatabaseErrorCode::QueryFailed, std::move(operation), begin);
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

        const auto versionResult = readCurrentVersion(m_database);
        if (std::holds_alternative<DatabaseError>(versionResult))
            return 0;
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
            return *error;

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
            return;

        const QString connectionName = std::move(m_connectionName);
        if (m_database.isValid())
            m_database.close();
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

} // namespace javelin::jmap::cache
