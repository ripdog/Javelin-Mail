#include "storage/migrations/DefaultMigrationSteps.h"

namespace javelin::jmap::cache::migrations
{
    std::vector<MigrationStep> migrationSteps70To78()
    {
        return {
            MigrationStep{
                .version = 70,
                .name = QStringLiteral("calendar_window_event_state"),
                .statements =
                    {
                        QStringLiteral(
                            "ALTER TABLE calendar_query_windows ADD COLUMN event_state TEXT"),
                    },
            },
            MigrationStep{
                .version = 71,
                .name = QStringLiteral("durable_calendar_alert_push_queue"),
                .statements =
                    {
                        QStringLiteral(
                            "CREATE TABLE calendar_pushed_alerts (push_key TEXT PRIMARY KEY, "
                            "owner_account_id TEXT NOT NULL REFERENCES accounts(account_id) ON "
                            "DELETE CASCADE, account_id TEXT NOT NULL, event_id TEXT NOT NULL, uid "
                            "TEXT NOT NULL, recurrence_id TEXT, alert_id TEXT NOT NULL, "
                            "received_at "
                            "TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP) STRICT"),
                        QStringLiteral("CREATE INDEX idx_calendar_pushed_alerts_owner ON "
                                       "calendar_pushed_alerts(owner_account_id)"),
                    },
            },
            MigrationStep{
                .version = 72,
                .name = QStringLiteral("calendar_reminder_horizon"),
                .statements =
                    {
                        QStringLiteral(
                            "CREATE TABLE calendar_reminder_horizons (account_id TEXT PRIMARY KEY "
                            "REFERENCES accounts(account_id) ON DELETE CASCADE, range_start TEXT "
                            "NOT NULL, range_end TEXT NOT NULL, display_time_zone TEXT NOT NULL, "
                            "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP) STRICT"),
                        QStringLiteral(
                            "CREATE TABLE calendar_reminder_occurrences (account_id TEXT NOT NULL, "
                            "occurrence_id TEXT NOT NULL, PRIMARY KEY(account_id,occurrence_id), "
                            "FOREIGN KEY(account_id) REFERENCES "
                            "calendar_reminder_horizons(account_id) "
                            "ON DELETE CASCADE, FOREIGN KEY(account_id,occurrence_id) REFERENCES "
                            "calendar_occurrences(account_id,occurrence_id) ON DELETE CASCADE) "
                            "STRICT"),
                    },
            },
            MigrationStep{
                .version = 73,
                .name = QStringLiteral("calendar_reminder_occurrence_timing"),
                .statements =
                    {
                        QStringLiteral(
                            "ALTER TABLE calendar_reminder_occurrences ADD COLUMN start_utc TEXT"),
                        QStringLiteral(
                            "ALTER TABLE calendar_reminder_occurrences ADD COLUMN end_utc TEXT"),
                    },
            },
            MigrationStep{
                .version = 74,
                .name = QStringLiteral("mail_cache_revision"),
                .statements =
                    {
                        QStringLiteral(
                            "CREATE TABLE mail_cache_revisions (account_id TEXT PRIMARY KEY "
                            "REFERENCES accounts(account_id) ON DELETE CASCADE, revision INTEGER "
                            "NOT NULL DEFAULT 0 CHECK(revision>=0), updated_at TEXT NOT NULL "
                            "DEFAULT "
                            "CURRENT_TIMESTAMP) STRICT"),
                        QStringLiteral(
                            "CREATE TRIGGER mail_cache_revision_email_insert AFTER INSERT ON "
                            "emails "
                            "BEGIN INSERT INTO mail_cache_revisions(account_id,revision) "
                            "VALUES(NEW.account_id,1) ON CONFLICT(account_id) DO UPDATE SET "
                            "revision=revision+1,updated_at=CURRENT_TIMESTAMP; END"),
                        QStringLiteral(
                            "CREATE TRIGGER mail_cache_revision_email_update AFTER UPDATE ON "
                            "emails "
                            "BEGIN INSERT INTO mail_cache_revisions(account_id,revision) "
                            "VALUES(NEW.account_id,1) ON CONFLICT(account_id) DO UPDATE SET "
                            "revision=revision+1,updated_at=CURRENT_TIMESTAMP; END"),
                        QStringLiteral(
                            "CREATE TRIGGER mail_cache_revision_email_delete AFTER DELETE ON "
                            "emails "
                            "BEGIN INSERT INTO mail_cache_revisions(account_id,revision) "
                            "VALUES(OLD.account_id,1) ON CONFLICT(account_id) DO UPDATE SET "
                            "revision=revision+1,updated_at=CURRENT_TIMESTAMP; END"),
                        QStringLiteral(
                            "CREATE TRIGGER mail_cache_revision_mailbox_window_insert AFTER INSERT "
                            "ON mailbox_query_windows BEGIN INSERT INTO "
                            "mail_cache_revisions(account_id,revision) VALUES(NEW.account_id,1) ON "
                            "CONFLICT(account_id) DO UPDATE SET revision=revision+1,"
                            "updated_at=CURRENT_TIMESTAMP; END"),
                        QStringLiteral(
                            "CREATE TRIGGER mail_cache_revision_mailbox_window_update AFTER UPDATE "
                            "ON mailbox_query_windows BEGIN INSERT INTO "
                            "mail_cache_revisions(account_id,revision) VALUES(NEW.account_id,1) ON "
                            "CONFLICT(account_id) DO UPDATE SET revision=revision+1,"
                            "updated_at=CURRENT_TIMESTAMP; END"),
                        QStringLiteral(
                            "CREATE TRIGGER mail_cache_revision_mailbox_window_delete AFTER DELETE "
                            "ON mailbox_query_windows BEGIN INSERT INTO "
                            "mail_cache_revisions(account_id,revision) VALUES(OLD.account_id,1) ON "
                            "CONFLICT(account_id) DO UPDATE SET revision=revision+1,"
                            "updated_at=CURRENT_TIMESTAMP; END"),
                        QStringLiteral(
                            "CREATE TRIGGER mail_cache_revision_search_window_insert AFTER INSERT "
                            "ON search_windows BEGIN INSERT INTO "
                            "mail_cache_revisions(account_id,revision) VALUES(NEW.account_id,1) ON "
                            "CONFLICT(account_id) DO UPDATE SET revision=revision+1,"
                            "updated_at=CURRENT_TIMESTAMP; END"),
                        QStringLiteral(
                            "CREATE TRIGGER mail_cache_revision_search_window_update AFTER UPDATE "
                            "ON search_windows BEGIN INSERT INTO "
                            "mail_cache_revisions(account_id,revision) VALUES(NEW.account_id,1) ON "
                            "CONFLICT(account_id) DO UPDATE SET revision=revision+1,"
                            "updated_at=CURRENT_TIMESTAMP; END"),
                        QStringLiteral(
                            "CREATE TRIGGER mail_cache_revision_search_window_delete AFTER DELETE "
                            "ON search_windows BEGIN INSERT INTO "
                            "mail_cache_revisions(account_id,revision) VALUES(OLD.account_id,1) ON "
                            "CONFLICT(account_id) DO UPDATE SET revision=revision+1,"
                            "updated_at=CURRENT_TIMESTAMP; END"),
                    },
            },
        };
    }
} // namespace javelin::jmap::cache::migrations
