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
        };
    }
} // namespace javelin::jmap::cache::migrations
