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
#include <QStackedWidget>
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
        constexpr auto remoteContentGroup = "remoteContent";
        constexpr auto allowedSendersKey = "allowedSenders";
        constexpr auto allowedDomainsKey = "allowedDomains";
        constexpr auto translationGroup = "translation";
        constexpr auto autoTranslateSendersKey = "autoTranslateSenders";
        constexpr auto autoTranslateDomainsKey = "autoTranslateDomains";
        constexpr int remoteContentKindRole = Qt::UserRole + 1;
        constexpr int remoteContentValueRole = Qt::UserRole + 2;
        constexpr int autoTranslateKindRole = Qt::UserRole + 1;
        constexpr int autoTranslateValueRole = Qt::UserRole + 2;

        enum class RemoteContentPermitKind
        {
            Sender,
            Domain,
        };

        enum class AutoTranslateEntryKind
        {
            Sender,
            Domain,
        };

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
                .sessionUrl = settings.value(QLatin1StringView{sessionUrlKey}).toString().trimmed(),
                .loginEmail = settings.value(QLatin1StringView{loginEmailKey}).toString().trimmed(),
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

        [[nodiscard]] QStringList remoteContentAllowList(const QLatin1StringView key)
        {
            QSettings settings;
            settings.beginGroup(QLatin1StringView{remoteContentGroup});
            auto values = settings.value(key).toStringList();
            settings.endGroup();
            values.removeAll(QString{});
            values.removeDuplicates();
            values.sort(Qt::CaseInsensitive);
            return values;
        }

        void saveRemoteContentAllowList(const QLatin1StringView key, QStringList values)
        {
            values.removeAll(QString{});
            values.removeDuplicates();
            values.sort(Qt::CaseInsensitive);

            QSettings settings;
            settings.beginGroup(QLatin1StringView{remoteContentGroup});
            settings.setValue(key, values);
            settings.endGroup();
            settings.sync();
        }

        [[nodiscard]] QStringList settingsList(const QLatin1StringView group,
                                               const QLatin1StringView key)
        {
            QSettings settings;
            settings.beginGroup(group);
            auto values = settings.value(key).toStringList();
            settings.endGroup();
            values.removeAll(QString{});
            values.removeDuplicates();
            values.sort(Qt::CaseInsensitive);
            return values;
        }

        void saveSettingsList(const QLatin1StringView group, const QLatin1StringView key,
                              QStringList values)
        {
            values.removeAll(QString{});
            values.removeDuplicates();
            values.sort(Qt::CaseInsensitive);

            QSettings settings;
            settings.beginGroup(group);
            settings.setValue(key, values);
            settings.endGroup();
            settings.sync();
        }
    } // namespace

    PreferencesDialog::PreferencesDialog(javelin::jmap::cache::AccountRepository& accountRepository,
                                         QWidget* parent)
        : QDialog(parent), m_accountRepository(accountRepository), m_accounts(loadAccounts())
    {
        setWindowTitle(QStringLiteral("Preferences"));
        resize(760, 420);

        auto* outerLayout = new QVBoxLayout(this);
        auto* contentLayout = new QHBoxLayout();

        m_pageList = new QListWidget(this);
        m_pageList->setFixedWidth(150);
        m_pageList->addItem(QStringLiteral("Accounts"));
        m_pageList->addItem(QStringLiteral("Remote Content"));
        m_pageList->addItem(QStringLiteral("Translation"));
        contentLayout->addWidget(m_pageList);

        m_pageStack = new QStackedWidget(this);

        auto* accountsPage = new QWidget(m_pageStack);
        auto* accountsPageLayout = new QVBoxLayout(accountsPage);
        auto* splitter = new QSplitter(accountsPage);
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
        accountsPageLayout->addWidget(splitter, 1);
        m_pageStack->addWidget(accountsPage);

        auto* remoteContentPage = new QWidget(m_pageStack);
        auto* remoteContentLayout = new QVBoxLayout(remoteContentPage);
        remoteContentLayout->addWidget(
            new QLabel(QStringLiteral("Allowed Remote Content"), remoteContentPage));
        m_remoteContentList = new QListWidget(remoteContentPage);
        m_remoteContentList->setSelectionMode(QAbstractItemView::ExtendedSelection);
        remoteContentLayout->addWidget(m_remoteContentList, 1);
        auto* remoteContentButtons = new QHBoxLayout();
        remoteContentButtons->addStretch(1);
        m_removeRemoteContentButton = new QPushButton(QStringLiteral("Remove"), remoteContentPage);
        remoteContentButtons->addWidget(m_removeRemoteContentButton);
        remoteContentLayout->addLayout(remoteContentButtons);
        m_pageStack->addWidget(remoteContentPage);

        auto* translationPage = new QWidget(m_pageStack);
        auto* translationLayout = new QVBoxLayout(translationPage);
        translationLayout->addWidget(
            new QLabel(QStringLiteral("Auto-Translate Entries"), translationPage));
        m_autoTranslateList = new QListWidget(translationPage);
        m_autoTranslateList->setSelectionMode(QAbstractItemView::ExtendedSelection);
        translationLayout->addWidget(m_autoTranslateList, 1);
        auto* translationButtons = new QHBoxLayout();
        translationButtons->addStretch(1);
        m_removeAutoTranslateButton = new QPushButton(QStringLiteral("Remove"), translationPage);
        translationButtons->addWidget(m_removeAutoTranslateButton);
        translationLayout->addLayout(translationButtons);
        m_pageStack->addWidget(translationPage);

        contentLayout->addWidget(m_pageStack, 1);
        outerLayout->addLayout(contentLayout, 1);

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
        connect(m_pageList, &QListWidget::currentRowChanged, m_pageStack,
                &QStackedWidget::setCurrentIndex);
        connect(m_removeRemoteContentButton, &QPushButton::clicked, this,
                &PreferencesDialog::removeSelectedRemoteContentPermits);
        connect(m_remoteContentList, &QListWidget::itemSelectionChanged, this,
                [this]
                {
                    m_removeRemoteContentButton->setEnabled(
                        !m_remoteContentList->selectedItems().empty());
                });
        connect(m_removeAutoTranslateButton, &QPushButton::clicked, this,
                &PreferencesDialog::removeSelectedAutoTranslateEntries);
        connect(m_autoTranslateList, &QListWidget::itemSelectionChanged, this,
                [this]
                {
                    m_removeAutoTranslateButton->setEnabled(
                        !m_autoTranslateList->selectedItems().empty());
                });
        outerLayout->addWidget(buttonBox);

        if (m_accounts.empty())
        {
            m_accounts.push_back(newAccount());
        }
        refreshAccountList();
        refreshRemoteContentList();
        refreshAutoTranslateList();
        m_pageList->setCurrentRow(0);
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
                .sessionUrl = settings.value(QLatin1StringView{sessionUrlKey}).toString().trimmed(),
                .loginEmail = settings.value(QLatin1StringView{loginEmailKey}).toString().trimmed(),
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
        const auto active = std::ranges::find(accounts, activeAccountId, &ConnectionSettings::id);
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
        settings.beginWriteArray(QLatin1StringView{sizeKey}, static_cast<int>(accounts.size()));
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
        const auto found =
            std::ranges::find(accounts, configuredAccountId, &ConnectionSettings::id);
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
        auto* confirmation = new QCheckBox(
            QStringLiteral("I understand that all cached data will be wiped."), &warning);
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

    void PreferencesDialog::refreshRemoteContentList()
    {
        m_remoteContentList->clear();

        const auto senders = remoteContentAllowList(QLatin1StringView{allowedSendersKey});
        for (const auto& sender : senders)
        {
            auto* item =
                new QListWidgetItem(QStringLiteral("Sender: %1").arg(sender), m_remoteContentList);
            item->setData(remoteContentKindRole, static_cast<int>(RemoteContentPermitKind::Sender));
            item->setData(remoteContentValueRole, sender);
        }

        const auto domains = remoteContentAllowList(QLatin1StringView{allowedDomainsKey});
        for (const auto& domain : domains)
        {
            auto* item =
                new QListWidgetItem(QStringLiteral("Domain: %1").arg(domain), m_remoteContentList);
            item->setData(remoteContentKindRole, static_cast<int>(RemoteContentPermitKind::Domain));
            item->setData(remoteContentValueRole, domain);
        }

        m_removeRemoteContentButton->setEnabled(false);
    }

    void PreferencesDialog::removeSelectedRemoteContentPermits()
    {
        auto senders = remoteContentAllowList(QLatin1StringView{allowedSendersKey});
        auto domains = remoteContentAllowList(QLatin1StringView{allowedDomainsKey});

        for (const auto* item : m_remoteContentList->selectedItems())
        {
            const auto kind =
                static_cast<RemoteContentPermitKind>(item->data(remoteContentKindRole).toInt());
            const auto value = item->data(remoteContentValueRole).toString();
            if (kind == RemoteContentPermitKind::Sender)
            {
                senders.removeAll(value);
            }
            else
            {
                domains.removeAll(value);
            }
        }

        saveRemoteContentAllowList(QLatin1StringView{allowedSendersKey}, senders);
        saveRemoteContentAllowList(QLatin1StringView{allowedDomainsKey}, domains);
        refreshRemoteContentList();
    }

    void PreferencesDialog::refreshAutoTranslateList()
    {
        m_autoTranslateList->clear();

        const auto senders = settingsList(QLatin1StringView{translationGroup},
                                          QLatin1StringView{autoTranslateSendersKey});
        for (const auto& sender : senders)
        {
            auto* item =
                new QListWidgetItem(QStringLiteral("Sender: %1").arg(sender), m_autoTranslateList);
            item->setData(autoTranslateKindRole, static_cast<int>(AutoTranslateEntryKind::Sender));
            item->setData(autoTranslateValueRole, sender);
        }

        const auto domains = settingsList(QLatin1StringView{translationGroup},
                                          QLatin1StringView{autoTranslateDomainsKey});
        for (const auto& domain : domains)
        {
            auto* item =
                new QListWidgetItem(QStringLiteral("Domain: %1").arg(domain), m_autoTranslateList);
            item->setData(autoTranslateKindRole, static_cast<int>(AutoTranslateEntryKind::Domain));
            item->setData(autoTranslateValueRole, domain);
        }

        m_removeAutoTranslateButton->setEnabled(false);
    }

    void PreferencesDialog::removeSelectedAutoTranslateEntries()
    {
        auto senders = settingsList(QLatin1StringView{translationGroup},
                                    QLatin1StringView{autoTranslateSendersKey});
        auto domains = settingsList(QLatin1StringView{translationGroup},
                                    QLatin1StringView{autoTranslateDomainsKey});

        for (const auto* item : m_autoTranslateList->selectedItems())
        {
            const auto kind =
                static_cast<AutoTranslateEntryKind>(item->data(autoTranslateKindRole).toInt());
            const auto value = item->data(autoTranslateValueRole).toString();
            if (kind == AutoTranslateEntryKind::Sender)
            {
                senders.removeAll(value);
            }
            else
            {
                domains.removeAll(value);
            }
        }

        saveSettingsList(QLatin1StringView{translationGroup},
                         QLatin1StringView{autoTranslateSendersKey}, senders);
        saveSettingsList(QLatin1StringView{translationGroup},
                         QLatin1StringView{autoTranslateDomainsKey}, domains);
        refreshAutoTranslateList();
    }

} // namespace javelin::gui::settings
