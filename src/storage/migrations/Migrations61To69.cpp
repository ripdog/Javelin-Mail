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
        };
    }
} // namespace javelin::jmap::cache::migrations
