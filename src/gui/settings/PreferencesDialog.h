#pragma once

#include <QDialog>

#include <vector>

class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;

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

    class PreferencesDialog : public QDialog
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
        static void saveAccounts(const std::vector<ConnectionSettings>& accounts);
        static void associateCachedAccount(const QString& configuredAccountId,
                                           const QString& cachedAccountId);

      private:
        void addAccount();
        void removeCurrentAccount();
        void selectAccount(int row);
        void storeCurrentEdits();
        void refreshAccountList();
        void refreshRemoteContentList();
        void removeSelectedRemoteContentPermits();
        void refreshAutoTranslateList();
        void removeSelectedAutoTranslateEntries();

        javelin::jmap::cache::AccountRepository& m_accountRepository;
        std::vector<ConnectionSettings> m_accounts;
        int m_currentRow = -1;
        QListWidget* m_pageList = nullptr;
        QListWidget* m_accountList = nullptr;
        QPushButton* m_removeButton = nullptr;
        QLineEdit* m_sessionUrlEdit = nullptr;
        QLineEdit* m_loginEmailEdit = nullptr;
        QLineEdit* m_apiKeyEdit = nullptr;
        QStackedWidget* m_pageStack = nullptr;
        QListWidget* m_remoteContentList = nullptr;
        QPushButton* m_removeRemoteContentButton = nullptr;
        QListWidget* m_autoTranslateList = nullptr;
        QPushButton* m_removeAutoTranslateButton = nullptr;
    };

} // namespace javelin::gui::settings
