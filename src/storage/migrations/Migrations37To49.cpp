#include "storage/migrations/DefaultMigrationSteps.h"

namespace javelin::jmap::cache::migrations
{
    std::vector<MigrationStep> migrationSteps37To49()
    {
        return {
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
                        QStringLiteral("CREATE TABLE mutation_journal_sequence ("
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
                        QStringLiteral("CREATE TABLE notification_dispatch_claims ("
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
                        QStringLiteral("CREATE INDEX idx_mail_vault_mailbox_refs_mailbox ON "
                                       "mail_vault_mailbox_refs(account_id,mailbox_id,email_id)"),
                        QStringLiteral("INSERT INTO "
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
            MigrationStep{
                .version = 42,
                .name = QStringLiteral("mail_vault_projection_content_hash_index"),
                .statements =
                    {
                        QStringLiteral("CREATE INDEX idx_mail_vault_projection_content_hash ON "
                                       "mail_vault_projection_jobs(content_hash,status)"),
                        QStringLiteral(
                            "CREATE INDEX idx_mail_vault_projection_target_pending ON "
                            "mail_vault_projection_jobs(status,account_id,mailbox_id,job_id)"),
                    },
            },
            MigrationStep{
                .version = 43,
                .name = QStringLiteral("mail_tag_definitions"),
                .statements =
                    {
                        QStringLiteral(
                            "CREATE TABLE mail_tag_definitions ("
                            "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON "
                            "DELETE "
                            "CASCADE,keyword TEXT NOT NULL COLLATE NOCASE,display_name TEXT "
                            "NOT "
                            "NULL,color TEXT NOT NULL DEFAULT '',sort_order INTEGER NOT NULL "
                            "DEFAULT 0,PRIMARY KEY(account_id,keyword)) STRICT"),
                        QStringLiteral("CREATE INDEX idx_mail_tag_definitions_order ON "
                                       "mail_tag_definitions(account_id,sort_order,display_name)"),
                    },
            },
            MigrationStep{
                .version = 44,
                .name = QStringLiteral("submission_delayed_send_capability"),
                .statements =
                    {
                        QStringLiteral(
                            "ALTER TABLE accounts ADD COLUMN submission_max_delayed_send "
                            "INTEGER NOT NULL DEFAULT 0 CHECK(submission_max_delayed_send>=0)"),
                    },
            },
            MigrationStep{
                .version = 45,
                .name = QStringLiteral("mail_vault_body_preview"),
                .statements =
                    {
                        QStringLiteral("ALTER TABLE mail_vault_email_refs ADD COLUMN "
                                       "body_preview TEXT"),
                    },
            },
            MigrationStep{
                .version = 46,
                .name = QStringLiteral("mailbox_create_projections"),
                .statements =
                    {
                        QStringLiteral(
                            "ALTER TABLE accounts ADD COLUMN mail_may_create_top_level_mailbox "
                            "INTEGER NOT NULL DEFAULT 0 "
                            "CHECK(mail_may_create_top_level_mailbox "
                            "IN (0,1))"),
                        QStringLiteral(
                            "CREATE TABLE mailbox_create_projections (account_id TEXT NOT "
                            "NULL REFERENCES accounts(account_id) ON DELETE "
                            "CASCADE,creation_id "
                            "TEXT NOT NULL,mutation_id TEXT NOT NULL UNIQUE,name TEXT NOT NULL,"
                            "parent_mailbox_id TEXT,sort_order INTEGER NOT NULL DEFAULT 0,"
                            "is_subscribed INTEGER NOT NULL DEFAULT 1 CHECK(is_subscribed IN "
                            "(0,1)),PRIMARY KEY(account_id,creation_id),FOREIGN "
                            "KEY(mutation_id) "
                            "REFERENCES mutation_journal(mutation_id) ON DELETE CASCADE) "
                            "STRICT"),
                        QStringLiteral("CREATE INDEX idx_mailbox_create_projection_account ON "
                                       "mailbox_create_projections(account_id,creation_id)"),
                    },
            },
            MigrationStep{
                .version = 47,
                .name = QStringLiteral("normalized_thread_membership"),
                .statements =
                    {
                        QStringLiteral(
                            "ALTER TABLE threads ADD COLUMN membership_freshness TEXT NOT "
                            "NULL DEFAULT 'current' CHECK(membership_freshness IN "
                            "('current','stale'))"),
                        QStringLiteral(
                            "ALTER TABLE threads ADD COLUMN member_count INTEGER NOT NULL "
                            "DEFAULT 0 CHECK(member_count>=0)"),
                        QStringLiteral("UPDATE threads SET "
                                       "member_count=json_array_length(email_ids_json)"),
                        QStringLiteral(
                            "CREATE TABLE thread_email_members (account_id TEXT NOT NULL,"
                            "thread_id TEXT NOT NULL,position INTEGER NOT NULL "
                            "CHECK(position>=0),"
                            "email_id TEXT NOT NULL,PRIMARY KEY(account_id,thread_id,email_id),"
                            "FOREIGN KEY(account_id,thread_id) REFERENCES "
                            "threads(account_id,thread_id) ON DELETE CASCADE) STRICT"),
                        QStringLiteral(
                            "INSERT INTO thread_email_members(account_id,thread_id,position,"
                            "email_id) SELECT t.account_id,t.thread_id,CAST(member.key AS "
                            "INTEGER),member.value FROM threads t CROSS JOIN "
                            "json_each(t.email_ids_json) member"),
                        QStringLiteral("CREATE UNIQUE INDEX idx_thread_email_members_position ON "
                                       "thread_email_members(account_id,thread_id,position)"),
                        QStringLiteral("CREATE INDEX idx_thread_email_members_email ON "
                                       "thread_email_members(account_id,email_id)"),
                        QStringLiteral("ALTER TABLE threads DROP COLUMN email_ids_json"),
                    },
            },
            MigrationStep{
                .version = 48,
                .name = QStringLiteral("mailbox_window_item_email_lookup"),
                .statements =
                    {
                        QStringLiteral("CREATE INDEX idx_mailbox_query_window_items_email ON "
                                       "mailbox_query_window_items(account_id,email_id)"),
                    },
            },
            MigrationStep{
                .version = 49,
                .name = QStringLiteral("email_summary_refresh_requests"),
                .statements =
                    {
                        QStringLiteral(
                            "CREATE TABLE email_summary_refresh_requests ("
                            "account_id TEXT NOT NULL REFERENCES accounts(account_id) ON "
                            "DELETE CASCADE,email_id TEXT NOT NULL,requested_at TEXT NOT NULL "
                            "DEFAULT CURRENT_TIMESTAMP,PRIMARY KEY(account_id,email_id)) "
                            "STRICT"),
                    },
            },
        };
    }
} // namespace javelin::jmap::cache::migrations
