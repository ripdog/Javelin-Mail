#pragma once

#include "app/AccountCredentialStore.h"
#include "protocol/SettingsContract.h"

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
        explicit SettingsRepository(std::unique_ptr<QSettings> settings,
                                    AccountCredentialStore* credentialStore = nullptr);

        [[nodiscard]] static std::unique_ptr<QSettings> canonicalSettings();

        [[nodiscard]] SettingsReadResult load();
        [[nodiscard]] javelin::protocol::SettingsUpdateReply
        update(javelin::protocol::UpdateSettingsRequest request);

      private:
        [[nodiscard]] std::optional<SettingsRepositoryError> migrateIfNeeded();
        [[nodiscard]] std::optional<SettingsRepositoryError> migrateLegacyCredentials();
        [[nodiscard]] SettingsReadResult readSnapshot(bool includeLegacyWorkspace = false);
        [[nodiscard]] std::optional<SettingsRepositoryError>
        writeSnapshot(const javelin::protocol::SettingsSnapshot& snapshot,
                      bool includeSchemaVersion = true);

        std::unique_ptr<QSettings> m_settings;
        AccountCredentialStore* m_credentialStore = nullptr;
    };

} // namespace javelin::app
