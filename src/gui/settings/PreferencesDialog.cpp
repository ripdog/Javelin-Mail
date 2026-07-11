#include "gui/settings/PreferencesDialog.h"

#include "jmap/cache/AccountRepository.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
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
        constexpr auto displayNameKey = "displayName";
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
        constexpr auto attachmentsGroup = "attachments";
        constexpr auto alwaysAskKey = "alwaysAsk";
        constexpr auto directoryKey = "directory";
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
                .displayName = {},
                .sessionUrl = {},
                .loginEmail = {},
                .apiKey = {},
                .cachedAccountIds = {},
            };
        }

        [[nodiscard]] QString accountListText(const ConnectionSettings& account)
        {
            if (!account.displayName.isEmpty() && !account.loginEmail.isEmpty() &&
                account.displayName.compare(account.loginEmail, Qt::CaseInsensitive) != 0)
            {
                return QStringLiteral("%1 — %2").arg(account.displayName, account.loginEmail);
            }
            if (!account.displayName.isEmpty())
            {
                return account.displayName;
            }
            return account.loginEmail.isEmpty() ? QStringLiteral("New account")
                                                : account.loginEmail;
        }

        [[nodiscard]] std::optional<ConnectionSettings> loadLegacyAccount()
        {
            QSettings settings;
            settings.beginGroup(QLatin1StringView{legacyConnectionGroup});
            const auto loginEmail =
                settings.value(QLatin1StringView{loginEmailKey}).toString().trimmed();
            const ConnectionSettings account{
                .id = QUuid::createUuid().toString(QUuid::WithoutBraces),
                .displayName = loginEmail,
                .sessionUrl = settings.value(QLatin1StringView{sessionUrlKey}).toString().trimmed(),
                .loginEmail = loginEmail,
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

        void saveAttachmentSaveSettings(const AttachmentSaveSettings& value)
        {
            QSettings settings;
            settings.beginGroup(QLatin1StringView{attachmentsGroup});
            settings.setValue(QLatin1StringView{alwaysAskKey}, value.alwaysAsk);
            settings.setValue(QLatin1StringView{directoryKey}, value.directory);
            settings.endGroup();
            settings.sync();
        }
    } // namespace

    PreferencesDialog::PreferencesDialog(javelin::jmap::cache::AccountRepository& accountRepository,
                                         QWidget* parent)
        : KConfigDialog(parent, QStringLiteral("preferences"), nullptr),
          m_accountRepository(accountRepository), m_accounts(loadAccounts()),
          m_remoteContentSenders(remoteContentAllowList(QLatin1StringView{allowedSendersKey})),
          m_remoteContentDomains(remoteContentAllowList(QLatin1StringView{allowedDomainsKey})),
          m_autoTranslateSenders(settingsList(QLatin1StringView{translationGroup},
                                              QLatin1StringView{autoTranslateSendersKey})),
          m_autoTranslateDomains(settingsList(QLatin1StringView{translationGroup},
                                              QLatin1StringView{autoTranslateDomainsKey})),
          m_attachmentSaveSettings(loadAttachmentSaveSettings())
    {
        setWindowTitle(QStringLiteral("Preferences"));
        resize(760, 420);

        auto* accountsPage = new QWidget(this);
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
        m_displayNameEdit = new QLineEdit(detailsPanel);
        m_displayNameEdit->setPlaceholderText(QStringLiteral("Personal"));
        m_sessionUrlEdit = new QLineEdit(detailsPanel);
        m_sessionUrlEdit->setPlaceholderText(
            QStringLiteral("https://api.fastmail.com/jmap/session"));
        m_loginEmailEdit = new QLineEdit(detailsPanel);
        m_loginEmailEdit->setPlaceholderText(QStringLiteral("name@example.com"));
        m_apiKeyEdit = new QLineEdit(detailsPanel);
        m_apiKeyEdit->setEchoMode(QLineEdit::Password);
        m_apiKeyEdit->setPlaceholderText(QStringLiteral("Paste API key"));
        m_sessionUrlEdit->setPlaceholderText(QStringLiteral("Optional — discover from email"));
        formLayout->addRow(QStringLiteral("Display Name"), m_displayNameEdit);
        formLayout->addRow(QStringLiteral("Server"), m_sessionUrlEdit);
        formLayout->addRow(QStringLiteral("Login Email"), m_loginEmailEdit);
        formLayout->addRow(QStringLiteral("API Key"), m_apiKeyEdit);
        detailsLayout->addLayout(formLayout);
        detailsLayout->addStretch();

        splitter->addWidget(accountPanel);
        splitter->addWidget(detailsPanel);
        splitter->setStretchFactor(1, 1);
        accountsPageLayout->addWidget(splitter, 1);
        addPage(accountsPage, QStringLiteral("Accounts"), QStringLiteral("user-identity"),
                QString{}, false);

        auto* remoteContentPage = new QWidget(this);
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
        addPage(remoteContentPage, QStringLiteral("Remote Content"),
                QStringLiteral("network-wireless-on"), QString{}, false);

        auto* translationPage = new QWidget(this);
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
        addPage(translationPage, QStringLiteral("Translation"),
                QStringLiteral("preferences-desktop-locale"), QString{}, false);

        auto* attachmentsPage = new QWidget(this);
        auto* attachmentsLayout = new QVBoxLayout(attachmentsPage);
        m_askAttachmentDirectoryRadio = new QRadioButton(
            QStringLiteral("Always ask where to save attachments"), attachmentsPage);
        m_saveAttachmentDirectoryRadio =
            new QRadioButton(QStringLiteral("Always save attachments to:"), attachmentsPage);
        attachmentsLayout->addWidget(m_askAttachmentDirectoryRadio);
        attachmentsLayout->addWidget(m_saveAttachmentDirectoryRadio);
        auto* attachmentDirectoryLayout = new QHBoxLayout();
        m_attachmentDirectoryEdit = new QLineEdit(attachmentsPage);
        m_attachmentDirectoryEdit->setReadOnly(true);
        m_attachmentDirectoryButton = new QPushButton(QStringLiteral("Choose..."), attachmentsPage);
        attachmentDirectoryLayout->addWidget(m_attachmentDirectoryEdit, 1);
        attachmentDirectoryLayout->addWidget(m_attachmentDirectoryButton);
        attachmentsLayout->addLayout(attachmentDirectoryLayout);
        attachmentsLayout->addStretch(1);
        m_askAttachmentDirectoryRadio->setChecked(m_attachmentSaveSettings.alwaysAsk);
        m_saveAttachmentDirectoryRadio->setChecked(!m_attachmentSaveSettings.alwaysAsk);
        m_attachmentDirectoryEdit->setText(m_attachmentSaveSettings.directory);
        addPage(attachmentsPage, QStringLiteral("Attachments"), QStringLiteral("mail-attachment"),
                QString{}, false);

        connect(addButton, &QPushButton::clicked, this, &PreferencesDialog::addAccount);
        connect(m_removeButton, &QPushButton::clicked, this,
                &PreferencesDialog::removeCurrentAccount);
        connect(m_accountList, &QListWidget::currentRowChanged, this,
                &PreferencesDialog::selectAccount);
        connect(m_displayNameEdit, &QLineEdit::textEdited, this,
                &PreferencesDialog::noteUnsavedChanges);
        connect(m_sessionUrlEdit, &QLineEdit::textEdited, this,
                &PreferencesDialog::noteUnsavedChanges);
        connect(m_loginEmailEdit, &QLineEdit::textEdited, this,
                &PreferencesDialog::noteUnsavedChanges);
        connect(m_apiKeyEdit, &QLineEdit::textEdited, this, &PreferencesDialog::noteUnsavedChanges);
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
        connect(m_askAttachmentDirectoryRadio, &QRadioButton::toggled, this,
                [this](const bool checked)
                {
                    if (!checked)
                    {
                        return;
                    }
                    m_attachmentSaveSettings.alwaysAsk = true;
                    updateAttachmentDirectoryControls();
                    noteUnsavedChanges();
                });
        connect(m_saveAttachmentDirectoryRadio, &QRadioButton::toggled, this,
                [this](const bool checked)
                {
                    if (!checked)
                    {
                        return;
                    }
                    m_attachmentSaveSettings.alwaysAsk = false;
                    updateAttachmentDirectoryControls();
                    noteUnsavedChanges();
                });
        connect(m_attachmentDirectoryButton, &QPushButton::clicked, this,
                &PreferencesDialog::selectAttachmentDirectory);

        if (m_accounts.empty())
        {
            m_accounts.push_back(newAccount());
        }
        for (const auto& account : m_accounts)
        {
            m_loadedAccountIds.push_back(account.id);
        }
        refreshAccountList();
        refreshRemoteContentList();
        refreshAutoTranslateList();
        updateAttachmentDirectoryControls();
        m_accountList->setCurrentRow(0);
        m_hasPendingChanges = false;
        updateButtons();
    }

    PreferencesDialog::~PreferencesDialog() = default;

    void PreferencesDialog::updateSettings()
    {
        if (!validateCurrentSettings())
        {
            return;
        }

        saveCurrentSettings();
        KConfigDialog::updateSettings();
    }

    bool PreferencesDialog::hasChanged()
    {
        return m_hasPendingChanges;
    }

    ConnectionSettings PreferencesDialog::settings() const
    {
        if (m_currentRow < 0 || m_currentRow >= static_cast<int>(m_accounts.size()))
        {
            return {};
        }
        auto result = m_accounts[static_cast<std::size_t>(m_currentRow)];
        result.displayName = m_displayNameEdit->text().trimmed();
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
            const auto loginEmail =
                settings.value(QLatin1StringView{loginEmailKey}).toString().trimmed();
            auto displayName =
                settings.value(QLatin1StringView{displayNameKey}).toString().trimmed();
            if (displayName.isEmpty())
            {
                displayName = loginEmail;
            }
            accounts.push_back(ConnectionSettings{
                .id = settings.value(QLatin1StringView{idKey}).toString(),
                .displayName = displayName,
                .sessionUrl = settings.value(QLatin1StringView{sessionUrlKey}).toString().trimmed(),
                .loginEmail = loginEmail,
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

    AttachmentSaveSettings PreferencesDialog::loadAttachmentSaveSettings()
    {
        QSettings settings;
        settings.beginGroup(QLatin1StringView{attachmentsGroup});
        const AttachmentSaveSettings value{
            .alwaysAsk = settings.value(QLatin1StringView{alwaysAskKey}, true).toBool(),
            .directory = settings.value(QLatin1StringView{directoryKey}).toString(),
        };
        settings.endGroup();
        return value;
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
            settings.setValue(QLatin1StringView{displayNameKey}, account.displayName);
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

    void PreferencesDialog::saveResolvedSessionUrl(const QString& configuredAccountId,
                                                   const QString& sessionUrl)
    {
        auto accounts = loadAccounts();
        const auto found =
            std::ranges::find(accounts, configuredAccountId, &ConnectionSettings::id);
        if (found == accounts.end() || sessionUrl.isEmpty())
        {
            return;
        }
        found->sessionUrl = sessionUrl;
        saveAccounts(accounts);
    }

    void PreferencesDialog::addAccount()
    {
        storeCurrentEdits();
        m_accounts.push_back(newAccount());
        noteUnsavedChanges();
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
                            QStringLiteral("Applying this change will permanently wipe all of this "
                                           "account's cached mail, drafts, state, and pending "
                                           "actions."),
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

        if (m_loadedAccountIds.contains(account.id))
        {
            m_removedAccounts.push_back(account);
        }
        m_accounts.erase(m_accounts.begin() + m_currentRow);
        m_currentRow = -1;
        refreshAccountList();
        m_accountList->setCurrentRow(m_accounts.empty() ? -1 : 0);
        noteUnsavedChanges();
    }

    void PreferencesDialog::selectAccount(const int row)
    {
        storeCurrentEdits();
        m_currentRow = row;
        const bool hasAccount = row >= 0 && row < static_cast<int>(m_accounts.size());
        m_removeButton->setEnabled(hasAccount);
        m_displayNameEdit->setEnabled(hasAccount);
        m_sessionUrlEdit->setEnabled(hasAccount);
        m_loginEmailEdit->setEnabled(hasAccount);
        m_apiKeyEdit->setEnabled(hasAccount);
        if (!hasAccount)
        {
            m_displayNameEdit->clear();
            m_sessionUrlEdit->clear();
            m_loginEmailEdit->clear();
            m_apiKeyEdit->clear();
            return;
        }

        const auto& account = m_accounts[static_cast<std::size_t>(row)];
        m_displayNameEdit->setText(account.displayName);
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
        account.displayName = m_displayNameEdit->text().trimmed();
        account.sessionUrl = m_sessionUrlEdit->text().trimmed();
        account.loginEmail = m_loginEmailEdit->text().trimmed();
        account.apiKey = m_apiKeyEdit->text().trimmed();
        if (m_accountList->count() > m_currentRow)
        {
            m_accountList->item(m_currentRow)->setText(accountListText(account));
        }
    }

    void PreferencesDialog::noteUnsavedChanges()
    {
        m_hasPendingChanges = true;
        updateButtons();
    }

    void PreferencesDialog::saveCurrentSettings()
    {
        storeCurrentEdits();
        for (const auto& account : m_removedAccounts)
        {
            if (const auto error = m_accountRepository.removeConfiguredAccount(
                    account.loginEmail, account.sessionUrl, account.cachedAccountIds))
            {
                QMessageBox::critical(this, QStringLiteral("Could not remove account"),
                                      error->message);
                return;
            }
        }
        m_removedAccounts.clear();

        saveAccounts(m_accounts);
        saveRemoteContentAllowList(QLatin1StringView{allowedSendersKey}, m_remoteContentSenders);
        saveRemoteContentAllowList(QLatin1StringView{allowedDomainsKey}, m_remoteContentDomains);
        saveSettingsList(QLatin1StringView{translationGroup},
                         QLatin1StringView{autoTranslateSendersKey}, m_autoTranslateSenders);
        saveSettingsList(QLatin1StringView{translationGroup},
                         QLatin1StringView{autoTranslateDomainsKey}, m_autoTranslateDomains);
        saveAttachmentSaveSettings(m_attachmentSaveSettings);

        QSettings settings;
        settings.beginGroup(QLatin1StringView{accountsGroup});
        settings.setValue(QLatin1StringView{activeAccountIdKey}, this->settings().id);
        settings.endGroup();
        settings.sync();
        m_loadedAccountIds.clear();
        for (const auto& account : m_accounts)
        {
            m_loadedAccountIds.push_back(account.id);
        }
        m_hasPendingChanges = false;
    }

    void PreferencesDialog::refreshAccountList()
    {
        m_accountList->clear();
        for (const auto& account : m_accounts)
        {
            m_accountList->addItem(accountListText(account));
        }
    }

    void PreferencesDialog::refreshRemoteContentList()
    {
        m_remoteContentList->clear();

        for (const auto& sender : m_remoteContentSenders)
        {
            auto* item =
                new QListWidgetItem(QStringLiteral("Sender: %1").arg(sender), m_remoteContentList);
            item->setData(remoteContentKindRole, static_cast<int>(RemoteContentPermitKind::Sender));
            item->setData(remoteContentValueRole, sender);
        }

        for (const auto& domain : m_remoteContentDomains)
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
        for (const auto* item : m_remoteContentList->selectedItems())
        {
            const auto kind =
                static_cast<RemoteContentPermitKind>(item->data(remoteContentKindRole).toInt());
            const auto value = item->data(remoteContentValueRole).toString();
            if (kind == RemoteContentPermitKind::Sender)
            {
                m_remoteContentSenders.removeAll(value);
            }
            else
            {
                m_remoteContentDomains.removeAll(value);
            }
        }

        refreshRemoteContentList();
        noteUnsavedChanges();
    }

    void PreferencesDialog::refreshAutoTranslateList()
    {
        m_autoTranslateList->clear();

        for (const auto& sender : m_autoTranslateSenders)
        {
            auto* item =
                new QListWidgetItem(QStringLiteral("Sender: %1").arg(sender), m_autoTranslateList);
            item->setData(autoTranslateKindRole, static_cast<int>(AutoTranslateEntryKind::Sender));
            item->setData(autoTranslateValueRole, sender);
        }

        for (const auto& domain : m_autoTranslateDomains)
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
        for (const auto* item : m_autoTranslateList->selectedItems())
        {
            const auto kind =
                static_cast<AutoTranslateEntryKind>(item->data(autoTranslateKindRole).toInt());
            const auto value = item->data(autoTranslateValueRole).toString();
            if (kind == AutoTranslateEntryKind::Sender)
            {
                m_autoTranslateSenders.removeAll(value);
            }
            else
            {
                m_autoTranslateDomains.removeAll(value);
            }
        }

        refreshAutoTranslateList();
        noteUnsavedChanges();
    }

    void PreferencesDialog::selectAttachmentDirectory()
    {
        const auto directory =
            QFileDialog::getExistingDirectory(this, QStringLiteral("Select Attachment Directory"),
                                              m_attachmentSaveSettings.directory);
        if (directory.isEmpty())
        {
            return;
        }

        m_attachmentSaveSettings.directory = directory;
        m_attachmentDirectoryEdit->setText(directory);
        noteUnsavedChanges();
    }

    bool PreferencesDialog::validateCurrentSettings()
    {
        if (m_attachmentSaveSettings.alwaysAsk)
        {
            return true;
        }

        if (!m_attachmentSaveSettings.directory.isEmpty() &&
            QDir{m_attachmentSaveSettings.directory}.exists())
        {
            return true;
        }

        QMessageBox::warning(this, QStringLiteral("Invalid attachment directory"),
                             QStringLiteral("Choose an existing directory for attachments, or "
                                            "select always asking where to save attachments."));
        m_attachmentDirectoryButton->setFocus();
        return false;
    }

    void PreferencesDialog::updateAttachmentDirectoryControls()
    {
        const bool useDirectory = !m_attachmentSaveSettings.alwaysAsk;
        m_attachmentDirectoryEdit->setEnabled(useDirectory);
        m_attachmentDirectoryButton->setEnabled(useDirectory);
    }

} // namespace javelin::gui::settings
