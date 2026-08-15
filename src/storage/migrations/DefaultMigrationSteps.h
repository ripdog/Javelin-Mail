#pragma once

#include "storage/migrations/MigrationRunner.h"

#include <vector>

namespace javelin::jmap::cache::migrations
{
    [[nodiscard]] std::vector<MigrationStep> migrationSteps1To13();
    [[nodiscard]] std::vector<MigrationStep> migrationSteps14To28();
    [[nodiscard]] std::vector<MigrationStep> migrationSteps29To36();
    [[nodiscard]] std::vector<MigrationStep> migrationSteps37To51();
} // namespace javelin::jmap::cache::migrations
