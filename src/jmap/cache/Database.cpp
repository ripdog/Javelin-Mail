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
                .message = operation + QStringLiteral(": ") + database.lastError().text(),
            };
        }

        [[nodiscard]] DatabaseError makeQueryError(const DatabaseErrorCode code,
                                                   const QString& operation, const QSqlQuery& query)
        {
            return DatabaseError{
                .code = code,
                .message = operation + QStringLiteral(": ") + query.lastError().text(),
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

        [[nodiscard]] std::optional<DatabaseError> applyPragmas(QSqlDatabase& database)
        {
            const std::vector<std::pair<QString, QString>> pragmas{
                {QStringLiteral("Enable foreign keys"), QStringLiteral("PRAGMA foreign_keys = ON")},
                {QStringLiteral("Enable WAL"), QStringLiteral("PRAGMA journal_mode = WAL")},
                {QStringLiteral("Reduce fsync pressure"),
                 QStringLiteral("PRAGMA synchronous = NORMAL")},
                {QStringLiteral("Configure busy timeout"),
                 QStringLiteral("PRAGMA busy_timeout = 5000")},
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
        if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")))
        {
            return DatabaseError{
                .code = DatabaseErrorCode::DriverUnavailable,
                .message = QStringLiteral("Qt SQLite driver is not available"),
            };
        }

        QSqlDatabase database =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), options.connectionName);
        database.setDatabaseName(options.databasePath);
        if (!database.open())
        {
            const auto error =
                makeError(DatabaseErrorCode::OpenFailed, QStringLiteral("Open database"), database);
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
                                "ON DELETE CASCADE, websocket_url TEXT NOT NULL, mode TEXT NOT NULL "
                                "CHECK(mode IN ('unknown','websocket','http_fallback')), "
                                "retry_after TEXT, last_error TEXT, updated_at TEXT NOT NULL DEFAULT "
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
            },
        };
    }

} // namespace javelin::jmap::cache
