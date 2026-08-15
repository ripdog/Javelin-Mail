#include "storage/migrations/DefaultMigrationSteps.h"

namespace javelin::jmap::cache::migrations
{
    std::vector<MigrationStep> migrationSteps52To60()
    {
        return {
            MigrationStep{
                .version = 52,
                .name = QStringLiteral("connection_qualified_account_identity"),
                .statements =
                    {
                        // account_id remains the stable local/cache key during the migration. The
                        // remote JMAP id is copied into an explicit column so future connections
                        // may use the same remote id without colliding in child tables.
                        QStringLiteral("ALTER TABLE accounts ADD COLUMN connection_id TEXT"),
                        QStringLiteral("ALTER TABLE accounts ADD COLUMN remote_account_id TEXT"),
                        QStringLiteral("UPDATE accounts SET remote_account_id=account_id WHERE "
                                       "remote_account_id IS NULL"),
                        QStringLiteral(
                            "CREATE UNIQUE INDEX idx_accounts_connection_remote ON accounts("
                            "connection_id,remote_account_id) WHERE connection_id IS NOT NULL AND "
                            "remote_account_id IS NOT NULL"),
                        QStringLiteral("CREATE INDEX idx_accounts_remote_id ON accounts("
                                       "remote_account_id,account_id)"),
                    },
            },
            MigrationStep{
                .version = 53,
                .name = QStringLiteral("mail_transfer_journal"),
                .statements =
                    {
                        QStringLiteral(
                            "CREATE TABLE mail_transfer_operations ("
                            "operation_id TEXT PRIMARY KEY,operation_group_id TEXT,"
                            "source_account_id TEXT NOT NULL,source_mailbox_id TEXT,"
                            "destination_account_id TEXT NOT NULL,destination_mailbox_id TEXT NOT "
                            "NULL,operation TEXT NOT NULL CHECK(operation IN ('copy','move')),"
                            "topology TEXT NOT NULL CHECK(topology IN "
                            "('same_session_copy','cross_server_import')),"
                            "status TEXT NOT NULL CHECK(status IN "
                            "('preparing','running','waiting_for_network','waiting_for_auth',"
                            "'waiting_for_space','blocked_unknown','partial','failed','cancelled',"
                            "'complete')),title TEXT NOT NULL DEFAULT '',last_error TEXT,"
                            "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                            "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP) STRICT"),
                        QStringLiteral("CREATE INDEX idx_mail_transfer_operations_recoverable ON "
                                       "mail_transfer_operations(status,created_at)"),
                        QStringLiteral(
                            "CREATE TABLE mail_transfer_items ("
                            "item_id TEXT PRIMARY KEY,operation_id TEXT NOT NULL REFERENCES "
                            "mail_transfer_operations(operation_id) ON DELETE CASCADE,"
                            "ordinal INTEGER NOT NULL,source_email_id TEXT NOT NULL,"
                            "source_blob_id TEXT NOT NULL,source_email_state TEXT,"
                            "source_mailbox_ids_json TEXT NOT NULL,source_keywords_json TEXT NOT "
                            "NULL,source_received_at TEXT,source_size INTEGER NOT NULL,"
                            "source_remove_mailbox_ids_json TEXT NOT NULL,source_destroy INTEGER "
                            "NOT "
                            "NULL CHECK(source_destroy IN (0,1)),raw_content_hash TEXT,"
                            "destination_creation_id TEXT NOT NULL,destination_upload_blob_id TEXT,"
                            "destination_email_id TEXT,destination_blob_id TEXT,"
                            "destination_thread_id TEXT,destination_size INTEGER,"
                            "reused_existing INTEGER NOT NULL DEFAULT 0 CHECK(reused_existing IN "
                            "(0,1)),destination_prior_mailbox_ids_json TEXT,"
                            "phase TEXT NOT NULL CHECK(phase IN "
                            "('prepared','acquiring_source','source_ready','uploading','uploaded',"
                            "'creating_destination','destination_unknown','destination_confirmed',"
                            "'removing_source','source_cleanup_unknown','partial_source_retained',"
                            "'failed','cancelled','complete')),last_error TEXT,"
                            "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                            "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                            "UNIQUE(operation_id,ordinal),UNIQUE(operation_id,source_email_id)) "
                            "STRICT"),
                        QStringLiteral("CREATE INDEX idx_mail_transfer_items_phase ON "
                                       "mail_transfer_items(operation_id,phase,ordinal)"),
                    },
            },
            MigrationStep{
                .version = 54,
                .name = QStringLiteral("mail_vault_pins"),
                .statements =
                    {
                        QStringLiteral(
                            "CREATE TABLE mail_vault_pins ("
                            "owner_kind TEXT NOT NULL,owner_id TEXT NOT NULL,content_hash TEXT NOT "
                            "NULL REFERENCES mail_vault_objects(content_hash) ON DELETE CASCADE,"
                            "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                            "PRIMARY KEY(owner_kind,owner_id,content_hash)) STRICT"),
                        QStringLiteral("CREATE INDEX idx_mail_vault_pins_hash ON "
                                       "mail_vault_pins(content_hash)"),
                    },
            },
        };
    }
} // namespace javelin::jmap::cache::migrations
