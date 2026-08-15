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
        };
    }
} // namespace javelin::jmap::cache::migrations
