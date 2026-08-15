#include "storage/migrations/DefaultMigrationSteps.h"

namespace javelin::jmap::cache::migrations
{
    std::vector<MigrationStep> migrationSteps37To51()
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
            MigrationStep{
                .version = 50,
                .name = QStringLiteral("calendar_invitation_notifications"),
                .statements =
                    {
                        QStringLiteral("ALTER TABLE calendar_state_tokens RENAME TO "
                                       "calendar_state_tokens_v49"),
                        QStringLiteral(
                            "CREATE TABLE calendar_state_tokens (account_id TEXT NOT NULL "
                            "REFERENCES accounts(account_id) ON DELETE CASCADE,data_type TEXT "
                            "NOT NULL CHECK(data_type IN ('Calendar','CalendarEvent',"
                            "'CalendarEventNotification')),state TEXT NOT NULL,PRIMARY KEY("
                            "account_id,data_type)) STRICT"),
                        QStringLiteral(
                            "INSERT INTO calendar_state_tokens(account_id,data_type,state) SELECT "
                            "account_id,data_type,state FROM calendar_state_tokens_v49"),
                        QStringLiteral("DROP TABLE calendar_state_tokens_v49"),
                        QStringLiteral("ALTER TABLE notification_dispatch_claims RENAME TO "
                                       "notification_dispatch_claims_v49"),
                        QStringLiteral(
                            "CREATE TABLE notification_dispatch_claims (kind TEXT NOT NULL "
                            "CHECK(kind IN ('mail','calendar','invitation')),claim_key TEXT NOT "
                            "NULL,claimed_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,PRIMARY KEY("
                            "kind,claim_key)) STRICT"),
                        QStringLiteral(
                            "INSERT INTO notification_dispatch_claims(kind,claim_key,claimed_at) "
                            "SELECT kind,claim_key,claimed_at FROM "
                            "notification_dispatch_claims_v49"),
                        QStringLiteral("DROP TABLE notification_dispatch_claims_v49"),
                        QStringLiteral(
                            "CREATE TABLE calendar_event_notifications (account_id TEXT NOT NULL "
                            "REFERENCES accounts(account_id) ON DELETE CASCADE,notification_id "
                            "TEXT NOT NULL,created TEXT,changed_by_name TEXT,changed_by_email TEXT,"
                            "changed_by_principal_id TEXT,changed_by_calendar_address TEXT,comment "
                            "TEXT,type TEXT CHECK(type IS NULL OR type IN "
                            "('created','updated','destroyed')),calendar_event_id TEXT,is_draft "
                            "INTEGER CHECK(is_draft IS NULL OR is_draft IN "
                            "(0,1)),event_document_json "
                            "TEXT,event_patch_json TEXT,is_deleted INTEGER NOT NULL DEFAULT 0 "
                            "CHECK(is_deleted IN (0,1)),PRIMARY KEY(account_id,notification_id)) "
                            "STRICT"),
                        QStringLiteral(
                            "CREATE INDEX idx_calendar_event_notifications_event ON "
                            "calendar_event_notifications(account_id,calendar_event_id,created)"),
                        QStringLiteral(
                            "CREATE TABLE calendar_pending_invitations (account_id TEXT NOT NULL,"
                            "event_id TEXT NOT NULL,self_participant_id TEXT NOT NULL,"
                            "source_notification_id TEXT,discovered_at TEXT NOT NULL DEFAULT "
                            "CURRENT_TIMESTAMP,last_seen_at TEXT NOT NULL DEFAULT "
                            "CURRENT_TIMESTAMP,"
                            "PRIMARY KEY(account_id,event_id),FOREIGN KEY(account_id,event_id) "
                            "REFERENCES calendar_events(account_id,event_id) ON DELETE CASCADE) "
                            "STRICT"),
                        QStringLiteral(
                            "CREATE INDEX idx_calendar_pending_invitations_discovered ON "
                            "calendar_pending_invitations(discovered_at,account_id,event_id)"),
                        QStringLiteral(
                            "CREATE TABLE calendar_invitation_outbox (invitation_key TEXT PRIMARY "
                            "KEY,account_id TEXT NOT NULL REFERENCES accounts(account_id) ON "
                            "DELETE "
                            "CASCADE,event_id TEXT NOT NULL,self_participant_id TEXT NOT NULL,"
                            "source_notification_id TEXT,discovered_at TEXT NOT NULL DEFAULT "
                            "CURRENT_TIMESTAMP,status TEXT NOT NULL DEFAULT 'pending' CHECK(status "
                            "IN ('pending','delivered','resolved')),created_at TEXT NOT NULL "
                            "DEFAULT "
                            "CURRENT_TIMESTAMP,delivered_at TEXT,resolved_at "
                            "TEXT,UNIQUE(account_id,"
                            "event_id)) STRICT"),
                        QStringLiteral(
                            "CREATE INDEX idx_calendar_invitation_outbox_status ON "
                            "calendar_invitation_outbox(status,created_at,invitation_key)"),
                        QStringLiteral(
                            "CREATE TABLE calendar_participant_identities (account_id TEXT NOT "
                            "NULL "
                            "REFERENCES accounts(account_id) ON DELETE CASCADE,identity_id TEXT "
                            "NOT "
                            "NULL,name TEXT NOT NULL DEFAULT '',calendar_address TEXT NOT NULL,"
                            "is_default INTEGER NOT NULL DEFAULT 0 CHECK(is_default IN (0,1)),"
                            "PRIMARY KEY(account_id,identity_id)) STRICT"),
                        QStringLiteral(
                            "CREATE INDEX idx_calendar_participant_identities_address ON "
                            "calendar_participant_identities(account_id,calendar_address)"),
                    },
            },
            MigrationStep{
                .version = 51,
                .name = QStringLiteral("calendar_invitation_occurrences"),
                .statements =
                    {
                        QStringLiteral("ALTER TABLE calendar_pending_invitations RENAME TO "
                                       "calendar_pending_invitations_v50"),
                        QStringLiteral(
                            "CREATE TABLE calendar_pending_invitations (account_id TEXT NOT NULL,"
                            "event_id TEXT NOT NULL,recurrence_id TEXT NOT NULL DEFAULT '',"
                            "self_participant_id TEXT NOT NULL,source_notification_id TEXT,"
                            "display_recurrence_id TEXT,display_start TEXT,display_utc_start TEXT,"
                            "discovered_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,last_seen_at "
                            "TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,PRIMARY "
                            "KEY(account_id,event_id,"
                            "recurrence_id),FOREIGN KEY(account_id,event_id) REFERENCES "
                            "calendar_events(account_id,event_id) ON DELETE CASCADE) STRICT"),
                        QStringLiteral(
                            "INSERT INTO calendar_pending_invitations(account_id,event_id,"
                            "recurrence_id,self_participant_id,source_notification_id,discovered_"
                            "at,"
                            "last_seen_at) SELECT account_id,event_id,'',self_participant_id,"
                            "source_notification_id,discovered_at,last_seen_at FROM "
                            "calendar_pending_invitations_v50"),
                        QStringLiteral("DROP TABLE calendar_pending_invitations_v50"),
                        QStringLiteral(
                            "CREATE INDEX idx_calendar_pending_invitations_discovered ON "
                            "calendar_pending_invitations(discovered_at,account_id,event_id,"
                            "recurrence_id)"),
                        QStringLiteral("ALTER TABLE calendar_invitation_outbox RENAME TO "
                                       "calendar_invitation_outbox_v50"),
                        QStringLiteral(
                            "CREATE TABLE calendar_invitation_outbox (invitation_key TEXT PRIMARY "
                            "KEY,account_id TEXT NOT NULL REFERENCES accounts(account_id) ON "
                            "DELETE "
                            "CASCADE,event_id TEXT NOT NULL,recurrence_id TEXT NOT NULL DEFAULT '',"
                            "self_participant_id TEXT NOT NULL,source_notification_id TEXT,"
                            "discovered_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,status TEXT NOT "
                            "NULL DEFAULT 'pending' CHECK(status IN "
                            "('pending','delivered','resolved')),created_at TEXT NOT NULL DEFAULT "
                            "CURRENT_TIMESTAMP,delivered_at TEXT,resolved_at "
                            "TEXT,UNIQUE(account_id,"
                            "event_id,recurrence_id)) STRICT"),
                        QStringLiteral(
                            "INSERT INTO calendar_invitation_outbox(invitation_key,account_id,"
                            "event_id,recurrence_id,self_participant_id,source_notification_id,"
                            "discovered_at,status,created_at,delivered_at,resolved_at) SELECT "
                            "invitation_key,account_id,event_id,'',self_participant_id,"
                            "source_notification_id,discovered_at,status,created_at,delivered_at,"
                            "resolved_at FROM calendar_invitation_outbox_v50"),
                        QStringLiteral("DROP TABLE calendar_invitation_outbox_v50"),
                        QStringLiteral(
                            "CREATE INDEX idx_calendar_invitation_outbox_status ON "
                            "calendar_invitation_outbox(status,created_at,invitation_key)"),
                    },
            },
        };
    }
} // namespace javelin::jmap::cache::migrations
