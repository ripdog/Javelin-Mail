#pragma once

#include "app/SettingsApplicationPorts.h"
#include "gui/messageview/MessageAppearance.h"
#include "gui/settings/ConnectionSettings.h"

#include <QMetaObject>
#include <QStringList>

#include <functional>
#include <optional>
#include <vector>

class QObject;

namespace javelin::gui::settings
{
    struct AttachmentSaveSettings
    {
        bool alwaysAsk = true;
        QString directory;
    };

    class GuiSettings final
    {
      public:
        explicit GuiSettings(javelin::app::SettingsPort& port);
        explicit GuiSettings(javelin::protocol::SettingsSnapshot snapshot);

        [[nodiscard]] const javelin::protocol::SettingsSnapshot& snapshot() const;
        [[nodiscard]] std::vector<ConnectionSettings> accounts() const;
        [[nodiscard]] ConnectionSettings accountForCachedId(QStringView accountId) const;
        [[nodiscard]] QStringList syncedMailboxIds(QStringView accountId) const;
        [[nodiscard]] QStringList notificationMailboxIds(QStringView accountId) const;
        [[nodiscard]] QStringList remoteContentSenders() const;
        [[nodiscard]] QStringList remoteContentDomains() const;
        [[nodiscard]] javelin::gui::messageview::MessageAppearanceSettings
        messageAppearanceSettings() const;
        [[nodiscard]] AttachmentSaveSettings attachmentSaveSettings() const;
        [[nodiscard]] int undoSendDelaySeconds() const;
        [[nodiscard]] bool undoSendUsesDialog() const;
        [[nodiscard]] const javelin::protocol::WorkspaceSettings& workspaceSettings() const;

        [[nodiscard]] std::optional<javelin::protocol::BoundaryError>
        update(javelin::protocol::SettingsUpdate update);
        [[nodiscard]] std::optional<javelin::protocol::BoundaryError>
        update(javelin::protocol::SettingsRevision baseRevision,
               javelin::protocol::SettingsUpdate update);
        /// Retries an update that was rejected as stale against the current revision. This is only
        /// safe when the sections the update replaces are identical in `baseline`, the snapshot the
        /// update was built from, and the current snapshot, so that only unrelated settings (such
        /// as the saved window state) changed in between.
        [[nodiscard]] std::optional<javelin::protocol::BoundaryError>
        updateRebased(const javelin::protocol::SettingsSnapshot& baseline,
                      javelin::protocol::SettingsUpdate update);
        [[nodiscard]] std::optional<javelin::protocol::BoundaryError>
        updateWorkspace(javelin::protocol::WorkspaceSettings workspace);
        [[nodiscard]] std::optional<javelin::protocol::BoundaryError>
        associateCachedAccount(const QString& configuredAccountId, const QString& cachedAccountId);
        [[nodiscard]] std::optional<javelin::protocol::BoundaryError>
        ensureNotificationMailboxSelected(const QString& accountId, const QString& mailboxId);
        [[nodiscard]] std::optional<javelin::protocol::BoundaryError>
        saveResolvedSessionUrl(const QString& configuredAccountId, const QString& sessionUrl);
        [[nodiscard]] QMetaObject::Connection connectChanged(QObject* context,
                                                             std::function<void()> callback);

        [[nodiscard]] static std::vector<javelin::protocol::AccountSettings>
        protocolAccounts(const std::vector<ConnectionSettings>& accounts);

      private:
        [[nodiscard]] static QStringList stringList(const std::vector<QString>& values);
        [[nodiscard]] static QStringList
        mailboxIds(const std::vector<javelin::protocol::MailboxSelectionSettings>& selections,
                   QStringView accountId);

        javelin::app::SettingsPort* m_port = nullptr;
        std::optional<javelin::protocol::SettingsSnapshot> m_localSnapshot;
    };
} // namespace javelin::gui::settings
