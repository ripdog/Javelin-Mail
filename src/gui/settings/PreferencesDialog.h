#pragma once

#include <KConfigDialog>

#include <vector>

class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;

namespace javelin::jmap::cache
{
    class AccountRepository;
}

namespace javelin::gui::settings
{

    struct ConnectionSettings
    {
        QString id;
        QString sessionUrl;
        QString loginEmail;
        QString apiKey;
        QStringList cachedAccountIds;
    };

    struct AttachmentSaveSettings
    {
        bool alwaysAsk = true;
        QString directory;
    };

    class PreferencesDialog : public KConfigDialog
    {
        Q_OBJECT

      public:
        explicit PreferencesDialog(javelin::jmap::cache::AccountRepository& accountRepository,
                                   QWidget* parent = nullptr);
        ~PreferencesDialog() override;

        [[nodiscard]] ConnectionSettings settings() const;

        [[nodiscard]] static std::vector<ConnectionSettings> loadAccounts();
        [[nodiscard]] static ConnectionSettings loadSettings();
        [[nodiscard]] static ConnectionSettings loadSettingsForAccount(QStringView accountId);
        [[nodiscard]] static AttachmentSaveSettings loadAttachmentSaveSettings();
        static void saveAccounts(const std::vector<ConnectionSettings>& accounts);
        static void associateCachedAccount(const QString& configuredAccountId,
                                           const QString& cachedAccountId);

      private:
        void updateSettings() override;
        [[nodiscard]] bool hasChanged() override;

        void addAccount();
        void removeCurrentAccount();
        void selectAccount(int row);
        void noteUnsavedChanges();
        void saveCurrentSettings();
        void storeCurrentEdits();
        void refreshAccountList();
        void refreshRemoteContentList();
        void removeSelectedRemoteContentPermits();
        void refreshAutoTranslateList();
        void removeSelectedAutoTranslateEntries();
        void selectAttachmentDirectory();
        [[nodiscard]] bool validateCurrentSettings();
        void updateAttachmentDirectoryControls();

        javelin::jmap::cache::AccountRepository& m_accountRepository;
        std::vector<ConnectionSettings> m_accounts;
        std::vector<ConnectionSettings> m_removedAccounts;
        QStringList m_loadedAccountIds;
        QStringList m_remoteContentSenders;
        QStringList m_remoteContentDomains;
        QStringList m_autoTranslateSenders;
        QStringList m_autoTranslateDomains;
        AttachmentSaveSettings m_attachmentSaveSettings;
        bool m_hasPendingChanges = false;
        int m_currentRow = -1;
        QListWidget* m_accountList = nullptr;
        QPushButton* m_removeButton = nullptr;
        QLineEdit* m_sessionUrlEdit = nullptr;
        QLineEdit* m_loginEmailEdit = nullptr;
        QLineEdit* m_apiKeyEdit = nullptr;
        QListWidget* m_remoteContentList = nullptr;
        QPushButton* m_removeRemoteContentButton = nullptr;
        QListWidget* m_autoTranslateList = nullptr;
        QPushButton* m_removeAutoTranslateButton = nullptr;
        QRadioButton* m_askAttachmentDirectoryRadio = nullptr;
        QRadioButton* m_saveAttachmentDirectoryRadio = nullptr;
        QLineEdit* m_attachmentDirectoryEdit = nullptr;
        QPushButton* m_attachmentDirectoryButton = nullptr;
    };

} // namespace javelin::gui::settings
