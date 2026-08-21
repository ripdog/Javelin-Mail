#include "storage/migrations/DefaultMigrationSteps.h"

namespace javelin::jmap::cache::migrations
{
    std::vector<MigrationStep> migrationSteps61To69()
    {
        return {
            MigrationStep{
                .version = 61,
                .name = QStringLiteral("calendar_participant_identity_state"),
                .statements =
                    {
                        QStringLiteral("ALTER TABLE calendar_state_tokens RENAME TO "
                                       "calendar_state_tokens_v60"),
                        QStringLiteral(
                            "CREATE TABLE calendar_state_tokens (account_id TEXT NOT NULL "
                            "REFERENCES accounts(account_id) ON DELETE CASCADE,data_type TEXT "
                            "NOT NULL CHECK(data_type IN ('Calendar','CalendarEvent',"
                            "'CalendarEventNotification','ParticipantIdentity')),state TEXT NOT "
                            "NULL,PRIMARY KEY(account_id,data_type)) STRICT"),
                        QStringLiteral(
                            "INSERT INTO calendar_state_tokens(account_id,data_type,state) SELECT "
                            "account_id,data_type,state FROM calendar_state_tokens_v60"),
                        QStringLiteral("DROP TABLE calendar_state_tokens_v60"),
                    },
            },
            MigrationStep{
                .version = 62,
                .name = QStringLiteral("mail_export_journal"),
                .statements =
                    {
                        QStringLiteral(
                            "CREATE TABLE mail_export_operations (operation_id TEXT PRIMARY KEY,"
                            "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON DELETE "
                            "CASCADE,scope_kind TEXT NOT NULL CHECK(scope_kind IN "
                            "('mailbox','account')),mailbox_id TEXT,format TEXT NOT NULL "
                            "CHECK(format IN ('eml','mboxrd')),destination_directory TEXT NOT NULL,"
                            "status TEXT NOT NULL CHECK(status IN ('preparing','running',"
                            "'waiting_network','waiting_auth','waiting_space','partial','failed',"
                            "'complete')),manifest_sealed INTEGER NOT NULL DEFAULT 0 "
                            "CHECK(manifest_sealed IN (0,1)),manifest_email_state TEXT,title TEXT "
                            "NOT NULL,created_at TEXT NOT NULL,last_error TEXT) STRICT"),
                        QStringLiteral("CREATE INDEX idx_mail_export_operations_recoverable ON "
                                       "mail_export_operations(status,created_at)"),
                        QStringLiteral(
                            "CREATE TABLE mail_export_mailboxes (operation_id TEXT NOT NULL "
                            "REFERENCES mail_export_operations(operation_id) ON DELETE CASCADE,"
                            "ordinal INTEGER NOT NULL,mailbox_id TEXT NOT NULL,display_name TEXT "
                            "NOT NULL,relative_path TEXT NOT NULL,PRIMARY KEY(operation_id,"
                            "mailbox_id),UNIQUE(operation_id,ordinal)) STRICT"),
                        QStringLiteral(
                            "CREATE TABLE mail_export_items (item_id TEXT PRIMARY KEY,operation_id "
                            "TEXT NOT NULL REFERENCES mail_export_operations(operation_id) ON "
                            "DELETE CASCADE,ordinal INTEGER NOT NULL,mailbox_id TEXT NOT NULL,"
                            "email_id TEXT NOT NULL,blob_id TEXT NOT NULL,size INTEGER NOT NULL,"
                            "subject TEXT,received_at TEXT NOT NULL,sender_name TEXT,sender_email "
                            "TEXT,phase TEXT NOT NULL CHECK(phase IN ('pending','source_ready',"
                            "'writing','complete','failed')),output_relative_path TEXT,"
                            "raw_content_hash TEXT,mbox_start_offset INTEGER,mbox_end_offset "
                            "INTEGER,"
                            "last_error TEXT,"
                            "UNIQUE(operation_id,mailbox_id,email_id),UNIQUE(operation_id,ordinal))"
                            " "
                            "STRICT"),
                        QStringLiteral("CREATE INDEX idx_mail_export_items_next ON "
                                       "mail_export_items(operation_id,phase,ordinal)"),
                    },
            },
            MigrationStep{
                .version = 63,
                .name = QStringLiteral("mutation_journal_terminal_retention"),
                .statements =
                    {
                        QStringLiteral(
                            "CREATE VIEW mutation_journal_retireable_terminal AS SELECT "
                            "m.mutation_id FROM mutation_journal m WHERE m.status IN "
                            "('accepted','rejected') AND NOT EXISTS(SELECT 1 FROM "
                            "mutation_journal a WHERE a.account_id=m.account_id AND "
                            "a.data_type=m.data_type AND a.object_id=m.object_id AND a.status IN "
                            "('pending','in_flight','unknown')) AND (m.operation_group_id IS NULL "
                            "OR NOT EXISTS(SELECT 1 FROM mutation_journal g WHERE "
                            "g.operation_group_id=m.operation_group_id AND g.status IN "
                            "('pending','in_flight','unknown'))) AND (m.operation_group_id IS NULL "
                            "OR (NOT EXISTS(SELECT 1 FROM operation_history h WHERE "
                            "h.operation_group_id=m.operation_group_id AND h.status IN "
                            "('preparing','executing_forward','executing_undo','executing_redo',"
                            "'blocked_unknown','blocked_partial')) AND NOT EXISTS(SELECT 1 FROM "
                            "background_jobs j WHERE j.job_id=m.operation_group_id AND j.status IN "
                            "('queued','running','paused','waiting_for_space','waiting_for_network'"
                            ","
                            "'waiting_for_auth')) AND NOT EXISTS(SELECT 1 FROM "
                            "mail_transfer_operations t WHERE (t.operation_group_id="
                            "m.operation_group_id OR t.operation_id=m.operation_group_id) AND "
                            "t.status IN ('preparing','running','waiting_for_network',"
                            "'waiting_for_auth','waiting_for_space','blocked_unknown','partial')))"
                            ")"),
                        QStringLiteral(
                            "DELETE FROM mutation_journal WHERE mutation_id IN (SELECT mutation_id "
                            "FROM mutation_journal_retireable_terminal)"),
                        QStringLiteral(
                            "CREATE TRIGGER mutation_journal_release_history_owner AFTER UPDATE OF "
                            "status ON operation_history WHEN OLD.status IN "
                            "('preparing','executing_forward','executing_undo','executing_redo',"
                            "'blocked_unknown','blocked_partial') AND NEW.status NOT IN "
                            "('preparing','executing_forward','executing_undo','executing_redo',"
                            "'blocked_unknown','blocked_partial') BEGIN DELETE FROM "
                            "mutation_journal "
                            "WHERE mutation_id IN (SELECT mutation_id FROM "
                            "mutation_journal_retireable_terminal); END"),
                        QStringLiteral(
                            "CREATE TRIGGER mutation_journal_release_history_delete AFTER DELETE "
                            "ON operation_history BEGIN DELETE FROM mutation_journal WHERE "
                            "mutation_id IN (SELECT mutation_id FROM "
                            "mutation_journal_retireable_terminal); END"),
                        QStringLiteral(
                            "CREATE TRIGGER mutation_journal_release_job_owner AFTER UPDATE OF "
                            "status ON background_jobs WHEN OLD.status NOT IN "
                            "('failed','complete') "
                            "AND NEW.status IN ('failed','complete') BEGIN DELETE FROM "
                            "mutation_journal WHERE mutation_id IN (SELECT mutation_id FROM "
                            "mutation_journal_retireable_terminal); END"),
                        QStringLiteral(
                            "CREATE TRIGGER mutation_journal_release_job_delete AFTER DELETE ON "
                            "background_jobs BEGIN DELETE FROM mutation_journal WHERE mutation_id "
                            "IN (SELECT mutation_id FROM mutation_journal_retireable_terminal); "
                            "END"),
                        QStringLiteral(
                            "CREATE TRIGGER mutation_journal_release_transfer_owner AFTER UPDATE "
                            "OF status ON mail_transfer_operations WHEN OLD.status NOT IN "
                            "('failed','cancelled','complete') AND NEW.status IN "
                            "('failed','cancelled','complete') BEGIN DELETE FROM mutation_journal "
                            "WHERE mutation_id IN (SELECT mutation_id FROM "
                            "mutation_journal_retireable_terminal); END"),
                        QStringLiteral(
                            "CREATE TRIGGER mutation_journal_release_transfer_delete AFTER DELETE "
                            "ON mail_transfer_operations BEGIN DELETE FROM mutation_journal WHERE "
                            "mutation_id IN (SELECT mutation_id FROM "
                            "mutation_journal_retireable_terminal); END"),
                    },
            },
        };
    }
} // namespace javelin::jmap::cache::migrations
