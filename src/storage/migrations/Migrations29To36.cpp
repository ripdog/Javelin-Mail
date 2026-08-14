#include "storage/migrations/DefaultMigrationSteps.h"

namespace javelin::jmap::cache::migrations
{
    std::vector<MigrationStep> migrationSteps29To36()
    {
        return {
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
                        QStringLiteral("CREATE TABLE operation_history ("
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
                        QStringLiteral("CREATE TABLE pending_sends ("
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
                        QStringLiteral("ALTER TABLE mailbox_query_windows DROP COLUMN is_valid"),
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
                        QStringLiteral("UPDATE mailbox_query_windows SET materialization='partial' "
                                       "WHERE coverage!='server'"),
                        QStringLiteral(
                            "ALTER TABLE search_windows ADD COLUMN materialization TEXT NOT "
                            "NULL DEFAULT 'complete' CHECK(materialization IN "
                            "('complete','partial'))"),
                        QStringLiteral("UPDATE search_windows SET materialization='partial' "
                                       "WHERE coverage!='server'"),
                    },
            },
        };
    }
} // namespace javelin::jmap::cache::migrations
