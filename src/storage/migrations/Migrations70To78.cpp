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
        };
    }
} // namespace javelin::jmap::cache::migrations
