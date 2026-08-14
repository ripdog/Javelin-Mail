#include "storage/migrations/DefaultMigrationSteps.h"

#include <iterator>
#include <utility>

namespace javelin::jmap::cache
{
    namespace
    {
        void appendSteps(std::vector<MigrationStep>& destination, std::vector<MigrationStep> source)
        {
            destination.insert(destination.end(), std::make_move_iterator(source.begin()),
                               std::make_move_iterator(source.end()));
        }
    } // namespace

    MigrationRunner createDefaultMigrationRunner()
    {
        std::vector<MigrationStep> steps;
        appendSteps(steps, migrations::migrationSteps1To13());
        appendSteps(steps, migrations::migrationSteps14To28());
        appendSteps(steps, migrations::migrationSteps29To36());
        appendSteps(steps, migrations::migrationSteps37To49());
        return MigrationRunner{std::move(steps)};
    }

} // namespace javelin::jmap::cache
