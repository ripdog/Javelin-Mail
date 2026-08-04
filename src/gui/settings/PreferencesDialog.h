#pragma once

#include "gui/messageview/MessageAppearance.h"
#include "gui/settings/ConnectionSettings.h"
#include "gui/settings/GuiSettings.h"
#include "gui/translation/TranslationTypes.h"

#include <KConfigDialog>
#include <QHash>

#include <cstdint>
#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;
class QComboBox;
class QSpinBox;

namespace javelin::gui::mailboxes
{
    class MailboxTreeModel;
    class MailboxTreeView;
} // namespace javelin::gui::mailboxes

namespace javelin::jmap::cache
{
    class AccountReader;
    class MailboxReader;
} // namespace javelin::jmap::cache

namespace javelin::app
{
    class AccountCommandPort;
    class OnboardingPort;
} // namespace javelin::app

namespace javelin::gui::translation
{
    class TranslationService;
}

namespace javelin::gui::settings
{

    class PreferencesDialog : public KConfigDialog
    {
        Q_OBJECT

      public:
        explicit PreferencesDialog(
            GuiSettings& settings, javelin::app::AccountCommandPort& accountCommandPort,
            javelin::app::OnboardingPort& onboardingPort,
            javelin::gui::translation::TranslationService& translationService,
            javelin::jmap::cache::AccountReader& accountReader,
            javelin::jmap::cache::MailboxReader& mailboxReader, QWidget* parent = nullptr);
        ~PreferencesDialog() override;

        void selectConfiguredAccount(const QString& connectionId);

      Q_SIGNALS:
        void accountAdded(const javelin::gui::settings::ConnectionSettings& settings);
        void accountReauthenticated(const javelin::gui::settings::ConnectionSettings& settings);

      private:
        void updateSettings() override;
        [[nodiscard]] bool hasChanged() override;

        void addAccount();
        void reauthenticateCurrentAccount();
        void removeCurrentAccount();
        void selectAccount(int row);
        void noteUnsavedChanges();
        [[nodiscard]] bool saveCurrentSettings();
        void storeCurrentEdits();
        void refreshAccountList();
        void refreshRemoteContentList();
        void removeSelectedRemoteContentPermits();
        void refreshAutoTranslateList();
        void removeSelectedAutoTranslateEntries();
        void updateTranslationControls();
        void updateLocalTranslationTargets();
        void refreshInstalledLocalModels();
        void downloadSelectedLocalModels();
        void removeSelectedLocalModels();
        void selectAttachmentDirectory();
        [[nodiscard]] bool validateCurrentSettings();
        void updateAttachmentDirectoryControls();
        void refreshMailboxSyncAccounts();
        void refreshMailboxSyncList();
        void storeMailboxSyncSelection();
        void storeMailboxNotificationSelection();

        GuiSettings& m_settings;
        javelin::protocol::SettingsRevision m_baseRevision;
        javelin::app::AccountCommandPort& m_accountCommandPort;
        javelin::app::OnboardingPort& m_onboardingPort;
        javelin::gui::translation::TranslationService& m_translationService;
        javelin::jmap::cache::AccountReader& m_accountReader;
        javelin::jmap::cache::MailboxReader& m_mailboxReader;
        std::vector<ConnectionSettings> m_accounts;
        std::vector<ConnectionSettings> m_removedAccounts;
        QStringList m_loadedAccountIds;
        QStringList m_remoteContentSenders;
        QStringList m_remoteContentDomains;
        javelin::gui::translation::TranslationSettings m_translationSettings;
        QStringList m_autoTranslateSenders;
        QStringList m_autoTranslateDomains;
        javelin::gui::messageview::MessageAppearanceSettings m_messageAppearanceSettings;
        AttachmentSaveSettings m_attachmentSaveSettings;
        int m_undoSendDelaySeconds = 10;
        bool m_hasPendingChanges = false;
        int m_currentRow = -1;
        QListWidget* m_accountList = nullptr;
        QPushButton* m_reauthenticateButton = nullptr;
        QPushButton* m_removeButton = nullptr;
        QLineEdit* m_displayNameEdit = nullptr;
        QLabel* m_loginEmailLabel = nullptr;
        QLabel* m_sessionUrlLabel = nullptr;
        QListWidget* m_remoteContentList = nullptr;
        QPushButton* m_removeRemoteContentButton = nullptr;
        QComboBox* m_translationProvider = nullptr;
        QWidget* m_translationControls = nullptr;
        QComboBox* m_translationTargetLanguage = nullptr;
        QWidget* m_googleTranslationControls = nullptr;
        QLineEdit* m_translationApiKeyEdit = nullptr;
        QWidget* m_localTranslationControls = nullptr;
        QComboBox* m_localTranslationSource = nullptr;
        QComboBox* m_localTranslationTarget = nullptr;
        QPushButton* m_downloadLocalModelsButton = nullptr;
        QListWidget* m_installedLocalModels = nullptr;
        QPushButton* m_removeLocalModelsButton = nullptr;
        QLabel* m_localModelStatus = nullptr;
        QListWidget* m_autoTranslateList = nullptr;
        QPushButton* m_removeAutoTranslateButton = nullptr;
        QComboBox* m_messageColorMode = nullptr;
        QRadioButton* m_askAttachmentDirectoryRadio = nullptr;
        QRadioButton* m_saveAttachmentDirectoryRadio = nullptr;
        QLineEdit* m_attachmentDirectoryEdit = nullptr;
        QPushButton* m_attachmentDirectoryButton = nullptr;
        QSpinBox* m_undoSendDelaySpinBox = nullptr;
        QComboBox* m_mailboxSyncAccount = nullptr;
        javelin::gui::mailboxes::MailboxTreeView* m_mailboxSyncList = nullptr;
        javelin::gui::mailboxes::MailboxTreeView* m_mailboxNotificationList = nullptr;
        javelin::gui::mailboxes::MailboxTreeModel* m_mailboxSyncModel = nullptr;
        javelin::gui::mailboxes::MailboxTreeModel* m_mailboxNotificationModel = nullptr;
        QHash<QString, QStringList> m_syncedMailboxIds;
        QHash<QString, QStringList> m_notificationMailboxIds;
        QString m_mailboxSyncCurrentAccountId;
    };

} // namespace javelin::gui::settings
