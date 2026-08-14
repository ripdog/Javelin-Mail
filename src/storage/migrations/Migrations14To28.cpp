#include "storage/migrations/DefaultMigrationSteps.h"

namespace javelin::jmap::cache::migrations
{
    std::vector<MigrationStep> migrationSteps14To28()
    {
        return {
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
                        QStringLiteral("CREATE INDEX idx_calendar_occurrences_range ON "
                                       "calendar_occurrences(account_id,local_start,local_end)"),
                        QStringLiteral("CREATE INDEX idx_calendar_membership_calendar ON "
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
                        QStringLiteral("CREATE UNIQUE INDEX idx_calendar_default_destination ON "
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
                        QStringLiteral("CREATE INDEX idx_mailbox_query_windows_mailbox ON "
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
        };
    }
} // namespace javelin::jmap::cache::migrations
