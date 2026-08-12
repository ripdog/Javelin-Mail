#pragma once

#include "storage/DatabaseError.h"

#include <QSqlDatabase>
#include <QString>

#include <optional>
#include <span>
#include <vector>

namespace javelin::jmap::cache
{
    struct MigrationStep
    {
        int version = 0;
        QString name;
        std::vector<QString> statements;
    };

    class MigrationRunner
    {
      public:
        explicit MigrationRunner(std::vector<MigrationStep> steps);

        [[nodiscard]] std::optional<DatabaseError> migrate(QSqlDatabase& database) const;
        [[nodiscard]] int latestVersion() const;
        [[nodiscard]] std::span<const MigrationStep> steps() const;

      private:
        std::vector<MigrationStep> m_steps;
    };

    [[nodiscard]] MigrationRunner createDefaultMigrationRunner();

} // namespace javelin::jmap::cache
