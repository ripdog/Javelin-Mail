#include "gui/settings/PreferencesDialog.h"

#include "jmap/cache/AccountRepository.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

namespace javelin::gui::settings
{
    namespace
    {
        constexpr auto accountsGroup = "accounts";
        constexpr auto legacyConnectionGroup = "connection";
        constexpr auto sizeKey = "size";
        constexpr auto activeAccountIdKey = "activeAccountId";
        constexpr auto idKey = "id";
        constexpr auto sessionUrlKey = "sessionUrl";
        constexpr auto loginEmailKey = "loginEmail";
        constexpr auto apiKeyKey = "apiKey";
        constexpr auto cachedAccountIdsKey = "cachedAccountIds";

        [[nodiscard]] ConnectionSettings newAccount()
        {
            return ConnectionSettings{
                .id = QUuid::createUuid().toString(QUuid::WithoutBraces),
                .sessionUrl = {},
                .loginEmail = {},
                .apiKey = {},
                .cachedAccountIds = {},
            };
        }

        [[nodiscard]] std::optional<ConnectionSettings> loadLegacyAccount()
        {
            QSettings settings;
            settings.beginGroup(QLatin1StringView{legacyConnectionGroup});
            const ConnectionSettings account{
                .id = QUuid::createUuid().toString(QUuid::WithoutBraces),
                .sessionUrl =
                    settings.value(QLatin1StringView{sessionUrlKey}).toString().trimmed(),
                .loginEmail =
                    settings.value(QLatin1StringView{loginEmailKey}).toString().trimmed(),
                .apiKey = settings.value(QLatin1StringView{apiKeyKey}).toString().trimmed(),
                .cachedAccountIds = {},
            };
            settings.endGroup();
            if (account.sessionUrl.isEmpty() && account.loginEmail.isEmpty() &&
                account.apiKey.isEmpty())
            {
                return std::nullopt;
            }
            return account;
        }
    } // namespace

    PreferencesDialog::PreferencesDialog(
        javelin::jmap::cache::AccountRepository& accountRepository, QWidget* parent)
        : QDialog(parent), m_accountRepository(accountRepository), m_accounts(loadAccounts())
    {
        setWindowTitle(QStringLiteral("Preferences"));
        resize(760, 420);

        auto* outerLayout = new QVBoxLayout(this);
        auto* splitter = new QSplitter(this);
        auto* accountPanel = new QWidget(splitter);
        auto* accountLayout = new QVBoxLayout(accountPanel);
        accountLayout->addWidget(new QLabel(QStringLiteral("Accounts"), accountPanel));
        m_accountList = new QListWidget(accountPanel);
        accountLayout->addWidget(m_accountList, 1);

        auto* accountButtons = new QHBoxLayout();
        auto* addButton = new QPushButton(QStringLiteral("Add"), accountPanel);
        m_removeButton = new QPushButton(QStringLiteral("Remove"), accountPanel);
        accountButtons->addWidget(addButton);
        accountButtons->addWidget(m_removeButton);
        accountLayout->addLayout(accountButtons);

        auto* detailsPanel = new QWidget(splitter);
        auto* detailsLayout = new QVBoxLayout(detailsPanel);
        auto* formLayout = new QFormLayout();
        m_sessionUrlEdit = new QLineEdit(detailsPanel);
        m_sessionUrlEdit->setPlaceholderText(
            QStringLiteral("https://api.fastmail.com/jmap/session"));
        m_loginEmailEdit = new QLineEdit(detailsPanel);
        m_loginEmailEdit->setPlaceholderText(QStringLiteral("name@example.com"));
        m_apiKeyEdit = new QLineEdit(detailsPanel);
        m_apiKeyEdit->setEchoMode(QLineEdit::Password);
        m_apiKeyEdit->setPlaceholderText(QStringLiteral("Paste API key"));
        formLayout->addRow(QStringLiteral("Session URL"), m_sessionUrlEdit);
        formLayout->addRow(QStringLiteral("Login Email"), m_loginEmailEdit);
        formLayout->addRow(QStringLiteral("API Key"), m_apiKeyEdit);
        detailsLayout->addLayout(formLayout);
        detailsLayout->addStretch();

        splitter->addWidget(accountPanel);
        splitter->addWidget(detailsPanel);
        splitter->setStretchFactor(1, 1);
        outerLayout->addWidget(splitter, 1);

        auto* buttonBox =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(buttonBox, &QDialogButtonBox::accepted, this,
                [this]
                {
                    storeCurrentEdits();
                    saveAccounts(m_accounts);
                    QSettings settings;
                    settings.beginGroup(QLatin1StringView{accountsGroup});
                    settings.setValue(QLatin1StringView{activeAccountIdKey}, this->settings().id);
                    settings.endGroup();
                    settings.sync();
                    accept();
                });
        connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(addButton, &QPushButton::clicked, this, &PreferencesDialog::addAccount);
        connect(m_removeButton, &QPushButton::clicked, this,
                &PreferencesDialog::removeCurrentAccount);
        connect(m_accountList, &QListWidget::currentRowChanged, this,
                &PreferencesDialog::selectAccount);
        outerLayout->addWidget(buttonBox);

        if (m_accounts.empty())
        {
            m_accounts.push_back(newAccount());
        }
        refreshAccountList();
        m_accountList->setCurrentRow(0);
    }

    PreferencesDialog::~PreferencesDialog() = default;

    ConnectionSettings PreferencesDialog::settings() const
    {
        if (m_currentRow < 0 || m_currentRow >= static_cast<int>(m_accounts.size()))
        {
            return {};
        }
        auto result = m_accounts[static_cast<std::size_t>(m_currentRow)];
        result.sessionUrl = m_sessionUrlEdit->text().trimmed();
        result.loginEmail = m_loginEmailEdit->text().trimmed();
        result.apiKey = m_apiKeyEdit->text().trimmed();
        return result;
    }

    std::vector<ConnectionSettings> PreferencesDialog::loadAccounts()
    {
        QSettings settings;
        settings.beginGroup(QLatin1StringView{accountsGroup});
        const int count = settings.beginReadArray(QLatin1StringView{sizeKey});
        std::vector<ConnectionSettings> accounts;
        accounts.reserve(static_cast<std::size_t>(count));
        for (int index = 0; index < count; ++index)
        {
            settings.setArrayIndex(index);
            accounts.push_back(ConnectionSettings{
                .id = settings.value(QLatin1StringView{idKey}).toString(),
                .sessionUrl =
                    settings.value(QLatin1StringView{sessionUrlKey}).toString().trimmed(),
                .loginEmail =
                    settings.value(QLatin1StringView{loginEmailKey}).toString().trimmed(),
                .apiKey = settings.value(QLatin1StringView{apiKeyKey}).toString().trimmed(),
                .cachedAccountIds =
                    settings.value(QLatin1StringView{cachedAccountIdsKey}).toStringList(),
            });
        }
        settings.endArray();
        settings.endGroup();
        if (accounts.empty())
        {
            const auto legacyAccount = loadLegacyAccount();
            if (legacyAccount.has_value())
            {
                accounts.push_back(*legacyAccount);
                saveAccounts(accounts);

                settings.beginGroup(QLatin1StringView{accountsGroup});
                settings.setValue(QLatin1StringView{activeAccountIdKey}, legacyAccount->id);
                settings.endGroup();
                settings.sync();
            }
        }
        return accounts;
    }

    ConnectionSettings PreferencesDialog::loadSettings()
    {
        const auto accounts = loadAccounts();
        if (accounts.empty())
        {
            return {};
        }

        QSettings settings;
        settings.beginGroup(QLatin1StringView{accountsGroup});
        const auto activeAccountId =
            settings.value(QLatin1StringView{activeAccountIdKey}).toString();
        settings.endGroup();
        const auto active =
            std::ranges::find(accounts, activeAccountId, &ConnectionSettings::id);
        return active == accounts.end() ? accounts.front() : *active;
    }

    ConnectionSettings PreferencesDialog::loadSettingsForAccount(const QStringView accountId)
    {
        const auto accounts = loadAccounts();
        const auto found = std::ranges::find_if(
            accounts, [accountId](const auto& account)
            { return account.cachedAccountIds.contains(accountId.toString()); });
        return found == accounts.end() ? ConnectionSettings{} : *found;
    }

    void PreferencesDialog::saveAccounts(const std::vector<ConnectionSettings>& accounts)
    {
        QSettings settings;
        settings.beginGroup(QLatin1StringView{accountsGroup});
        settings.beginWriteArray(QLatin1StringView{sizeKey},
                                 static_cast<int>(accounts.size()));
        for (int index = 0; index < static_cast<int>(accounts.size()); ++index)
        {
            settings.setArrayIndex(index);
            const auto& account = accounts[static_cast<std::size_t>(index)];
            settings.setValue(QLatin1StringView{idKey}, account.id);
            settings.setValue(QLatin1StringView{sessionUrlKey}, account.sessionUrl);
            settings.setValue(QLatin1StringView{loginEmailKey}, account.loginEmail);
            settings.setValue(QLatin1StringView{apiKeyKey}, account.apiKey);
            settings.setValue(QLatin1StringView{cachedAccountIdsKey}, account.cachedAccountIds);
        }
        settings.endArray();
        settings.endGroup();
        settings.sync();
    }

    void PreferencesDialog::associateCachedAccount(const QString& configuredAccountId,
                                                   const QString& cachedAccountId)
    {
        auto accounts = loadAccounts();
        const auto found = std::ranges::find(accounts, configuredAccountId,
                                             &ConnectionSettings::id);
        if (found == accounts.end())
        {
            return;
        }
        if (!found->cachedAccountIds.contains(cachedAccountId))
        {
            found->cachedAccountIds.push_back(cachedAccountId);
            saveAccounts(accounts);
        }
    }

    void PreferencesDialog::addAccount()
    {
        storeCurrentEdits();
        m_accounts.push_back(newAccount());
        refreshAccountList();
        m_accountList->setCurrentRow(static_cast<int>(m_accounts.size()) - 1);
        m_loginEmailEdit->setFocus();
    }

    void PreferencesDialog::removeCurrentAccount()
    {
        if (m_currentRow < 0 || m_currentRow >= static_cast<int>(m_accounts.size()))
        {
            return;
        }

        const auto& account = m_accounts[static_cast<std::size_t>(m_currentRow)];
        QMessageBox warning{QMessageBox::Warning, QStringLiteral("Remove account"),
                            QStringLiteral("Removing this account permanently wipes all of its "
                                           "cached mail, drafts, state, and pending actions."),
                            QMessageBox::Cancel, this};
        auto* removeButton =
            warning.addButton(QStringLiteral("Remove Account"), QMessageBox::DestructiveRole);
        auto* confirmation =
            new QCheckBox(QStringLiteral("I understand that all cached data will be wiped."),
                          &warning);
        confirmation->setChecked(false);
        removeButton->setEnabled(false);
        connect(confirmation, &QCheckBox::toggled, removeButton, &QPushButton::setEnabled);
        warning.setCheckBox(confirmation);
        warning.exec();
        if (warning.clickedButton() != removeButton)
        {
            return;
        }

        if (const auto error = m_accountRepository.removeConfiguredAccount(
                account.loginEmail, account.sessionUrl, account.cachedAccountIds))
        {
            QMessageBox::critical(this, QStringLiteral("Could not remove account"), error->message);
            return;
        }

        m_accounts.erase(m_accounts.begin() + m_currentRow);
        saveAccounts(m_accounts);
        QSettings settings;
        settings.beginGroup(QLatin1StringView{accountsGroup});
        settings.setValue(QLatin1StringView{activeAccountIdKey},
                          m_accounts.empty() ? QString{} : m_accounts.front().id);
        settings.endGroup();
        settings.sync();
        m_currentRow = -1;
        refreshAccountList();
        m_accountList->setCurrentRow(m_accounts.empty() ? -1 : 0);
    }

    void PreferencesDialog::selectAccount(const int row)
    {
        storeCurrentEdits();
        m_currentRow = row;
        const bool hasAccount = row >= 0 && row < static_cast<int>(m_accounts.size());
        m_removeButton->setEnabled(hasAccount);
        m_sessionUrlEdit->setEnabled(hasAccount);
        m_loginEmailEdit->setEnabled(hasAccount);
        m_apiKeyEdit->setEnabled(hasAccount);
        if (!hasAccount)
        {
            m_sessionUrlEdit->clear();
            m_loginEmailEdit->clear();
            m_apiKeyEdit->clear();
            return;
        }

        const auto& account = m_accounts[static_cast<std::size_t>(row)];
        m_sessionUrlEdit->setText(account.sessionUrl);
        m_loginEmailEdit->setText(account.loginEmail);
        m_apiKeyEdit->setText(account.apiKey);
    }

    void PreferencesDialog::storeCurrentEdits()
    {
        if (m_currentRow < 0 || m_currentRow >= static_cast<int>(m_accounts.size()))
        {
            return;
        }
        auto& account = m_accounts[static_cast<std::size_t>(m_currentRow)];
        account.sessionUrl = m_sessionUrlEdit->text().trimmed();
        account.loginEmail = m_loginEmailEdit->text().trimmed();
        account.apiKey = m_apiKeyEdit->text().trimmed();
        if (m_accountList->count() > m_currentRow)
        {
            m_accountList->item(m_currentRow)
                ->setText(account.loginEmail.isEmpty() ? QStringLiteral("New account")
                                                       : account.loginEmail);
        }
    }

    void PreferencesDialog::refreshAccountList()
    {
        m_accountList->clear();
        for (const auto& account : m_accounts)
        {
            m_accountList->addItem(account.loginEmail.isEmpty() ? QStringLiteral("New account")
                                                                : account.loginEmail);
        }
    }

} // namespace javelin::gui::settings
