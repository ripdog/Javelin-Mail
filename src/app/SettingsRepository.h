#pragma once

#include "protocol/ProcessBoundary.h"

#include <QSettings>

#include <memory>
#include <variant>

namespace javelin::app
{

    enum class SettingsRepositoryErrorCode
    {
        ReadFailed,
        InvalidStoredValue,
        UnsupportedSchema,
        MigrationFailed,
        WriteFailed,
    };

    struct SettingsRepositoryError
    {
        SettingsRepositoryErrorCode code = SettingsRepositoryErrorCode::ReadFailed;
        QString key;
        QString detail;
    };

    using SettingsReadResult =
        std::variant<javelin::protocol::SettingsSnapshot, SettingsRepositoryError>;

    class SettingsRepository final
    {
      public:
        SettingsRepository();
        explicit SettingsRepository(std::unique_ptr<QSettings> settings);

        [[nodiscard]] static std::unique_ptr<QSettings> canonicalSettings();

        [[nodiscard]] SettingsReadResult load();
        [[nodiscard]] javelin::protocol::SettingsUpdateReply
        update(javelin::protocol::UpdateSettingsRequest request);

      private:
        [[nodiscard]] std::optional<SettingsRepositoryError> migrateIfNeeded();
        [[nodiscard]] SettingsReadResult readSnapshot();
        [[nodiscard]] std::optional<SettingsRepositoryError>
        writeSnapshot(const javelin::protocol::SettingsSnapshot& snapshot);

        std::unique_ptr<QSettings> m_settings;
    };

} // namespace javelin::app
