#pragma once

#include <KConfigDialog>
#include <QHash>
#include <QSet>

#include <vector>

class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;
class QComboBox;

namespace javelin::jmap::cache
{
    class AccountRepository;
    class QueryService;
} // namespace javelin::jmap::cache

namespace javelin::gui::settings
{

    struct ConnectionSettings
    {
        QString id;
        QString displayName;
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
                                   javelin::jmap::cache::QueryService& queryService,
                                   QWidget* parent = nullptr);
        ~PreferencesDialog() override;

        [[nodiscard]] ConnectionSettings settings() const;

        [[nodiscard]] static std::vector<ConnectionSettings> loadAccounts();
        [[nodiscard]] static ConnectionSettings loadSettingsForAccount(QStringView accountId);
        [[nodiscard]] static AttachmentSaveSettings loadAttachmentSaveSettings();
        [[nodiscard]] static QStringList syncedMailboxIds(QStringView accountId);
        [[nodiscard]] static QStringList notificationMailboxIds(QStringView accountId);
        [[nodiscard]] static bool hasNotificationMailboxSelection(QStringView accountId);
        static void saveAccounts(const std::vector<ConnectionSettings>& accounts);
        static void associateCachedAccount(const QString& configuredAccountId,
                                           const QString& cachedAccountId);
        static void saveResolvedSessionUrl(const QString& configuredAccountId,
                                           const QString& sessionUrl);

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
        void refreshMailboxSyncAccounts();
        void refreshMailboxSyncList();
        void storeMailboxSyncSelection();
        void storeMailboxNotificationSelection();

        javelin::jmap::cache::AccountRepository& m_accountRepository;
        javelin::jmap::cache::QueryService& m_queryService;
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
        QLineEdit* m_displayNameEdit = nullptr;
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
        QComboBox* m_mailboxSyncAccount = nullptr;
        QListWidget* m_mailboxSyncList = nullptr;
        QListWidget* m_mailboxNotificationList = nullptr;
        QHash<QString, QStringList> m_syncedMailboxIds;
        QHash<QString, QStringList> m_notificationMailboxIds;
        QSet<QString> m_configuredNotificationAccounts;
        QString m_mailboxSyncCurrentAccountId;
    };

} // namespace javelin::gui::settings
