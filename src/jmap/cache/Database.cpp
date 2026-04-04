#include "jmap/cache/Database.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QThread>

#include <cstdint>
#include <span>
#include <sstream>
#include <utility>

namespace javelin::jmap::cache
{

    namespace
    {

        [[nodiscard]] DatabaseError makeError(const DatabaseErrorCode code,
                                              const QString& operation,
                                              const QSqlDatabase& database)
        {
            return DatabaseError{
                .code = code,
                .message = operation + ": " + database.lastError().text(),
            };
        }

        [[nodiscard]] DatabaseError makeQueryError(const DatabaseErrorCode code,
                                                   const QString& operation, const QSqlQuery& query)
        {
            return DatabaseError{
                .code = code,
                .message = operation + ": " + query.lastError().text(),
            };
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
            return executeStatement(database,
                                    "CREATE TABLE IF NOT EXISTS schema_migrations ("
                                    "version INTEGER PRIMARY KEY,"
                                    "name TEXT NOT NULL,"
                                    "applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
                                    ") STRICT",
                                    "Create schema_migrations");
        }

        [[nodiscard]] std::variant<int, DatabaseError>
        readCurrentVersion(const QSqlDatabase& database)
        {
            QSqlQuery query{database};
            if (!query.exec("SELECT COALESCE(MAX(version), 0) FROM schema_migrations"))
            {
                return makeQueryError(DatabaseErrorCode::QueryFailed, "Read schema version", query);
            }

            if (!query.next())
            {
                return 0;
            }

            return query.value(0).toInt();
        }

        [[nodiscard]] std::optional<DatabaseError> applyPragmas(QSqlDatabase& database)
        {
            const std::vector<std::pair<QString, QString>> pragmas{
                {"Enable foreign keys", "PRAGMA foreign_keys = ON"},
                {"Enable WAL", "PRAGMA journal_mode = WAL"},
                {"Reduce fsync pressure", "PRAGMA synchronous = NORMAL"},
                {"Configure busy timeout", "PRAGMA busy_timeout = 5000"},
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
                .message = "Migration steps must have strictly increasing versions",
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
                return makeError(DatabaseErrorCode::MigrationFailed, "Begin migration transaction",
                                 database);
            }

            std::optional<DatabaseError> failure;
            for (const auto& statement : step.statements)
            {
                if (const auto error =
                        executeStatement(database, statement,
                                         "Apply migration " + QString::number(step.version) + " (" +
                                             step.name + ")"))
                {
                    failure = error;
                    break;
                }
            }

            if (!failure.has_value())
            {
                QSqlQuery insertQuery{database};
                insertQuery.prepare(
                    "INSERT INTO schema_migrations (version, name) VALUES (:version, :name)");
                insertQuery.bindValue(":version", step.version);
                insertQuery.bindValue(":name", step.name);
                if (!insertQuery.exec())
                {
                    failure = makeQueryError(DatabaseErrorCode::MigrationFailed,
                                             "Record schema migration", insertQuery);
                }
            }

            if (!failure.has_value() && !database.commit())
            {
                failure =
                    makeError(DatabaseErrorCode::MigrationFailed, "Commit migration", database);
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

    DatabaseConnection::DatabaseConnection(QString connectionName, QSqlDatabase database)
        : m_connectionName(std::move(connectionName)), m_database(std::move(database))
    {
    }

    DatabaseConnection::DatabaseConnection(DatabaseConnection&& other) noexcept
        : m_connectionName(std::move(other.m_connectionName)),
          m_database(std::move(other.m_database))
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
        other.m_database = QSqlDatabase{};
        other.m_connectionName.clear();
        return *this;
    }

    DatabaseConnection::~DatabaseConnection()
    {
        reset();
    }

    std::variant<DatabaseConnection, DatabaseError>
    DatabaseConnection::open(const DatabaseConnectionOptions& options)
    {
        if (!QSqlDatabase::isDriverAvailable("QSQLITE"))
        {
            return DatabaseError{
                .code = DatabaseErrorCode::DriverUnavailable,
                .message = "Qt SQLite driver is not available",
            };
        }

        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", options.connectionName);
        database.setDatabaseName(options.databasePath);
        if (!database.open())
        {
            const auto error = makeError(DatabaseErrorCode::OpenFailed, "Open database", database);
            database.close();
            database = QSqlDatabase{};
            return error;
        }

        if (const auto pragmaError = applyPragmas(database))
        {
            database.close();
            database = QSqlDatabase{};
            return *pragmaError;
        }

        const auto migrationRunner = createDefaultMigrationRunner();
        if (const auto migrationError = migrationRunner.migrate(database))
        {
            database.close();
            database = QSqlDatabase{};
            return *migrationError;
        }

        return DatabaseConnection{options.connectionName, database};
    }

    QSqlDatabase& DatabaseConnection::database()
    {
        return m_database;
    }

    const QSqlDatabase& DatabaseConnection::database() const
    {
        return m_database;
    }

    const QString& DatabaseConnection::connectionName() const
    {
        return m_connectionName;
    }

    std::optional<DatabaseError> DatabaseConnection::validate() const
    {
        if (!m_database.isValid() || !m_database.isOpen())
        {
            return DatabaseError{
                .code = DatabaseErrorCode::OpenFailed,
                .message = "Database connection is not open",
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

    std::variant<std::vector<AppliedMigration>, DatabaseError>
    DatabaseConnection::appliedMigrations() const
    {
        if (const auto error = validate())
        {
            return *error;
        }

        QSqlQuery query{m_database};
        if (!query.exec("SELECT version, name FROM schema_migrations ORDER BY version"))
        {
            return makeQueryError(DatabaseErrorCode::QueryFailed, "Read schema_migrations", query);
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

        if (m_database.isValid())
        {
            m_database.close();
        }

        m_database = QSqlDatabase{};
        m_connectionName.clear();
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
        });
    }

    QString ThreadConnectionFactory::makeConnectionName(const std::string_view ownerTag) const
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
                    .name = "initial_cache_schema",
                    .statements =
                        {
                            "CREATE TABLE IF NOT EXISTS accounts ("
                            "account_id TEXT PRIMARY KEY,"
                            "email_address TEXT NOT NULL,"
                            "session_url TEXT NOT NULL,"
                            "is_primary INTEGER NOT NULL DEFAULT 0,"
                            "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                            "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
                            ") STRICT",
                            "CREATE TABLE IF NOT EXISTS sessions ("
                            "account_id TEXT PRIMARY KEY REFERENCES accounts(account_id) ON "
                            "DELETE CASCADE,"
                            "api_url TEXT NOT NULL,"
                            "download_url TEXT,"
                            "upload_url TEXT,"
                            "event_source_url TEXT,"
                            "state TEXT,"
                            "username TEXT NOT NULL"
                            ") STRICT",
                            "CREATE TABLE IF NOT EXISTS mailboxes ("
                            "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON DELETE "
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
                            ") STRICT",
                            "CREATE TABLE IF NOT EXISTS threads ("
                            "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON DELETE "
                            "CASCADE,"
                            "thread_id TEXT NOT NULL,"
                            "email_ids_json TEXT NOT NULL DEFAULT '[]',"
                            "state TEXT,"
                            "PRIMARY KEY (account_id, thread_id)"
                            ") STRICT",
                            "CREATE TABLE IF NOT EXISTS emails ("
                            "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON DELETE "
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
                            ") STRICT",
                            "CREATE TABLE IF NOT EXISTS email_mailboxes ("
                            "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON DELETE "
                            "CASCADE,"
                            "email_id TEXT NOT NULL,"
                            "mailbox_id TEXT NOT NULL,"
                            "PRIMARY KEY (account_id, email_id, mailbox_id)"
                            ") STRICT",
                            "CREATE TABLE IF NOT EXISTS email_keywords ("
                            "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON DELETE "
                            "CASCADE,"
                            "email_id TEXT NOT NULL,"
                            "keyword TEXT NOT NULL,"
                            "PRIMARY KEY (account_id, email_id, keyword)"
                            ") STRICT",
                            "CREATE TABLE IF NOT EXISTS email_addresses ("
                            "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON DELETE "
                            "CASCADE,"
                            "email_id TEXT NOT NULL,"
                            "field_name TEXT NOT NULL,"
                            "position INTEGER NOT NULL,"
                            "display_name TEXT,"
                            "address TEXT NOT NULL,"
                            "PRIMARY KEY (account_id, email_id, field_name, position)"
                            ") STRICT",
                            "CREATE TABLE IF NOT EXISTS email_body_values ("
                            "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON DELETE "
                            "CASCADE,"
                            "email_id TEXT NOT NULL,"
                            "part_id TEXT NOT NULL,"
                            "blob_id TEXT,"
                            "is_truncated INTEGER NOT NULL DEFAULT 0,"
                            "value TEXT NOT NULL,"
                            "PRIMARY KEY (account_id, email_id, part_id)"
                            ") STRICT",
                            "CREATE TABLE IF NOT EXISTS email_parts ("
                            "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON DELETE "
                            "CASCADE,"
                            "email_id TEXT NOT NULL,"
                            "part_id TEXT NOT NULL,"
                            "parent_part_id TEXT,"
                            "blob_id TEXT,"
                            "kind TEXT NOT NULL,"
                            "media_type TEXT NOT NULL,"
                            "name TEXT,"
                            "charset TEXT,"
                            "disposition TEXT,"
                            "cid TEXT,"
                            "size INTEGER NOT NULL DEFAULT 0,"
                            "is_inline_renderable INTEGER NOT NULL DEFAULT 0,"
                            "is_body_section INTEGER NOT NULL DEFAULT 0,"
                            "PRIMARY KEY (account_id, email_id, part_id)"
                            ") STRICT",
                            "CREATE TABLE IF NOT EXISTS identities ("
                            "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON DELETE "
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
                            ") STRICT",
                            "CREATE TABLE IF NOT EXISTS submissions ("
                            "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON DELETE "
                            "CASCADE,"
                            "submission_id TEXT NOT NULL,"
                            "email_id TEXT NOT NULL,"
                            "thread_id TEXT,"
                            "envelope_json TEXT NOT NULL DEFAULT '{}',"
                            "undo_status TEXT,"
                            "delivery_status TEXT,"
                            "state TEXT,"
                            "PRIMARY KEY (account_id, submission_id)"
                            ") STRICT",
                            "CREATE TABLE IF NOT EXISTS sync_state ("
                            "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON DELETE "
                            "CASCADE,"
                            "object_type TEXT NOT NULL,"
                            "query_key TEXT NOT NULL DEFAULT '',"
                            "state_token TEXT NOT NULL,"
                            "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                            "PRIMARY KEY (account_id, object_type, query_key)"
                            ") STRICT",
                            "CREATE TABLE IF NOT EXISTS pending_actions ("
                            "pending_action_id TEXT PRIMARY KEY,"
                            "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON DELETE "
                            "CASCADE,"
                            "action_type TEXT NOT NULL,"
                            "status TEXT NOT NULL,"
                            "payload_json TEXT NOT NULL,"
                            "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                            "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
                            ") STRICT",
                            "CREATE TABLE IF NOT EXISTS notifications ("
                            "notification_id TEXT PRIMARY KEY,"
                            "account_id TEXT REFERENCES accounts(account_id) ON DELETE CASCADE,"
                            "kind TEXT NOT NULL,"
                            "payload_json TEXT NOT NULL,"
                            "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                            "read_at TEXT"
                            ") STRICT",
                            "CREATE TABLE IF NOT EXISTS settings ("
                            "scope TEXT NOT NULL,"
                            "key TEXT NOT NULL,"
                            "value_json TEXT NOT NULL,"
                            "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                            "PRIMARY KEY (scope, key)"
                            ") STRICT",
                            "CREATE INDEX IF NOT EXISTS idx_mailboxes_parent ON mailboxes "
                            "(account_id, parent_mailbox_id, sort_order, mailbox_id)",
                            "CREATE INDEX IF NOT EXISTS idx_emails_thread ON emails (account_id, "
                            "thread_id)",
                            "CREATE INDEX IF NOT EXISTS idx_emails_received ON emails "
                            "(account_id, received_at DESC, email_id)",
                            "CREATE INDEX IF NOT EXISTS idx_email_mailboxes_mailbox ON "
                            "email_mailboxes (account_id, mailbox_id, email_id)",
                            "CREATE INDEX IF NOT EXISTS idx_email_keywords_keyword ON "
                            "email_keywords (account_id, keyword, email_id)",
                            "CREATE INDEX IF NOT EXISTS idx_sync_state_object ON sync_state "
                            "(account_id, object_type, query_key)",
                            "CREATE INDEX IF NOT EXISTS idx_pending_actions_status ON "
                            "pending_actions (account_id, status, created_at)",
                            "CREATE INDEX IF NOT EXISTS idx_email_parts_email ON email_parts "
                            "(account_id, email_id, part_id)",
                            "CREATE INDEX IF NOT EXISTS idx_email_parts_blob ON email_parts "
                            "(account_id, blob_id)",
                        },
                },
                MigrationStep{
                    .version = 2,
                    .name = "mailboxes_is_subscribed",
                    .statements =
                        {
                            "ALTER TABLE mailboxes ADD COLUMN is_subscribed INTEGER NOT NULL "
                            "DEFAULT 0",
                        },
                },
                MigrationStep{
                    .version = 3,
                    .name = "session_and_account_metadata",
                    .statements =
                        {
                            "ALTER TABLE accounts ADD COLUMN name TEXT NOT NULL DEFAULT ''",
                            "ALTER TABLE accounts ADD COLUMN is_personal INTEGER NOT NULL DEFAULT "
                            "0",
                            "ALTER TABLE accounts ADD COLUMN is_read_only INTEGER NOT NULL DEFAULT "
                            "0",
                            "ALTER TABLE accounts ADD COLUMN cap_mail INTEGER NOT NULL DEFAULT 0",
                            "ALTER TABLE accounts ADD COLUMN cap_submission INTEGER NOT NULL "
                            "DEFAULT 0",
                            "ALTER TABLE sessions ADD COLUMN has_core_capability INTEGER NOT NULL "
                            "DEFAULT 0",
                            "ALTER TABLE sessions ADD COLUMN has_mail_capability INTEGER NOT NULL "
                            "DEFAULT 0",
                            "ALTER TABLE sessions ADD COLUMN has_submission_capability INTEGER NOT "
                            "NULL DEFAULT 0",
                            "ALTER TABLE sessions ADD COLUMN core_capabilities_json TEXT NOT NULL "
                            "DEFAULT 'null'",
                            "ALTER TABLE sessions ADD COLUMN primary_mail_account_id TEXT",
                            "ALTER TABLE sessions ADD COLUMN primary_submission_account_id TEXT",
                        },
                },
                MigrationStep{
                    .version = 4,
                    .name = "email_parts_metadata",
                    .statements =
                        {
                            "CREATE TABLE IF NOT EXISTS email_parts ("
                            "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON DELETE "
                            "CASCADE,"
                            "email_id TEXT NOT NULL,"
                            "part_id TEXT NOT NULL,"
                            "parent_part_id TEXT,"
                            "blob_id TEXT,"
                            "kind TEXT NOT NULL,"
                            "media_type TEXT NOT NULL,"
                            "name TEXT,"
                            "charset TEXT,"
                            "disposition TEXT,"
                            "cid TEXT,"
                            "size INTEGER NOT NULL DEFAULT 0,"
                            "is_inline_renderable INTEGER NOT NULL DEFAULT 0,"
                            "is_body_section INTEGER NOT NULL DEFAULT 0,"
                            "PRIMARY KEY (account_id, email_id, part_id)"
                            ") STRICT",
                            "CREATE INDEX IF NOT EXISTS idx_email_parts_email ON email_parts "
                            "(account_id, email_id, part_id)",
                            "CREATE INDEX IF NOT EXISTS idx_email_parts_blob ON email_parts "
                            "(account_id, blob_id)",
                        },
                },
            },
        };
    }

} // namespace javelin::jmap::cache
