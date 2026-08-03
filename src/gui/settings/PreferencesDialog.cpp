#include "gui/settings/PreferencesDialog.h"

#include "app/AccountApplicationPorts.h"
#include "gui/mailboxes/MailboxTreeModel.h"
#include "gui/mailboxes/MailboxTreeView.h"
#include "gui/onboarding/FirstRunWizard.h"
#include "jmap/cache/AccountReadRepository.h"
#include "jmap/cache/MailboxReadRepository.h"

#include <QCheckBox>
#include <QComboBox>
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
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

namespace javelin::gui::settings
{
    namespace
    {
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
                .revision = 1,
                .displayName = {},
                .sessionUrl = {},
                .loginEmail = {},
                .apiKey = {},
                .refreshToken = {},
                .tokenEndpoint = {},
                .oauthClientId = {},
                .tokenExpiresAtEpochSeconds = 0,
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

        [[nodiscard]] QString selectedTranslationLanguageCode(const QComboBox& comboBox)
        {
            const int index = comboBox.currentIndex();
            if (index >= 0 && comboBox.currentText() == comboBox.itemText(index))
            {
                const auto code = comboBox.itemData(index).toString();
                if (!code.isEmpty())
                {
                    return code;
                }
            }
            return comboBox.currentText().trimmed();
        }

    } // namespace

    PreferencesDialog::PreferencesDialog(GuiSettings& settings,
                                         javelin::app::AccountCommandPort& accountCommandPort,
                                         javelin::app::OnboardingPort& onboardingPort,
                                         javelin::jmap::cache::AccountReader& accountReader,
                                         javelin::jmap::cache::MailboxReader& mailboxReader,
                                         QWidget* parent)
        : KConfigDialog(parent, QStringLiteral("preferences"), nullptr), m_settings(settings),
          m_baseRevision(m_settings.snapshot().revision), m_accountCommandPort(accountCommandPort),
          m_onboardingPort(onboardingPort), m_accountReader(accountReader),
          m_mailboxReader(mailboxReader), m_accounts(m_settings.accounts()),
          m_remoteContentSenders(m_settings.remoteContentSenders()),
          m_remoteContentDomains(m_settings.remoteContentDomains()),
          m_translationSettings(m_settings.translationSettings()),
          m_autoTranslateSenders(m_translationSettings.autoTranslateSenders),
          m_autoTranslateDomains(m_translationSettings.autoTranslateDomains),
          m_messageAppearanceSettings(m_settings.messageAppearanceSettings()),
          m_attachmentSaveSettings(m_settings.attachmentSaveSettings()),
          m_undoSendDelaySeconds(m_settings.undoSendDelaySeconds())
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

        auto* mailboxSyncPage = new QWidget(this);
        auto* mailboxSyncLayout = new QVBoxLayout(mailboxSyncPage);
        mailboxSyncLayout->addWidget(new QLabel(
            QStringLiteral(
                "Download every message and attachment in selected mailboxes for "
                "complete offline access. Large mailboxes continue in the Task Center "
                "and can be paused. Unchecking keeps downloaded mail as removable cache."),
            mailboxSyncPage));
        m_mailboxSyncAccount = new QComboBox(mailboxSyncPage);
        mailboxSyncLayout->addWidget(m_mailboxSyncAccount);
        m_mailboxSyncList = new javelin::gui::mailboxes::MailboxTreeView(mailboxSyncPage);
        m_mailboxSyncModel = new javelin::gui::mailboxes::MailboxTreeModel(
            m_accountReader, m_mailboxReader,
            {.accountId = std::string{},
             .showAccount = false,
             .checkable = true,
             .checkedMailboxIds = {},
             .accountDisplayName = [this](const QStringView accountId)
             { return m_settings.accountForCachedId(accountId).displayName; }},
            m_mailboxSyncList);
        m_mailboxSyncList->setModel(m_mailboxSyncModel);
        auto* mailboxLists = new QHBoxLayout();
        auto* syncListLayout = new QVBoxLayout();
        syncListLayout->addWidget(
            new QLabel(QStringLiteral("Keep complete offline copy"), mailboxSyncPage));
        syncListLayout->addWidget(m_mailboxSyncList, 1);
        mailboxLists->addLayout(syncListLayout, 1);
        auto* notificationListLayout = new QVBoxLayout();
        notificationListLayout->addWidget(
            new QLabel(QStringLiteral("Show notifications"), mailboxSyncPage));
        m_mailboxNotificationList = new javelin::gui::mailboxes::MailboxTreeView(mailboxSyncPage);
        m_mailboxNotificationModel = new javelin::gui::mailboxes::MailboxTreeModel(
            m_accountReader, m_mailboxReader,
            {.accountId = std::string{},
             .showAccount = false,
             .checkable = true,
             .checkedMailboxIds = {},
             .accountDisplayName = [this](const QStringView accountId)
             { return m_settings.accountForCachedId(accountId).displayName; }},
            m_mailboxNotificationList);
        m_mailboxNotificationList->setModel(m_mailboxNotificationModel);
        notificationListLayout->addWidget(m_mailboxNotificationList, 1);
        mailboxLists->addLayout(notificationListLayout, 1);
        mailboxSyncLayout->addLayout(mailboxLists, 1);
        addPage(mailboxSyncPage, QStringLiteral("Mailbox Sync"), QStringLiteral("view-refresh"),
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

        auto* appearancePage = new QWidget(this);
        auto* appearanceLayout = new QFormLayout(appearancePage);
        m_messageColorMode = new QComboBox(appearancePage);
        m_messageColorMode->addItem(
            QStringLiteral("Follow application"),
            static_cast<int>(javelin::gui::messageview::MessageColorMode::FollowApplication));
        m_messageColorMode->addItem(
            QStringLiteral("Always use original colours"),
            static_cast<int>(javelin::gui::messageview::MessageColorMode::Light));
        m_messageColorMode->addItem(
            QStringLiteral("Always use dark colours"),
            static_cast<int>(javelin::gui::messageview::MessageColorMode::Dark));
        const int messageColorModeIndex =
            m_messageColorMode->findData(static_cast<int>(m_messageAppearanceSettings.colorMode));
        m_messageColorMode->setCurrentIndex(messageColorModeIndex);
        appearanceLayout->addRow(QStringLiteral("HTML message colours"), m_messageColorMode);
        addPage(appearancePage, QStringLiteral("Appearance"),
                QStringLiteral("preferences-desktop-theme"), QString{}, false);

        auto* translationPage = new QWidget(this);
        auto* translationLayout = new QVBoxLayout(translationPage);
        m_translationEnabledCheckBox =
            new QCheckBox(QStringLiteral("Enable message translation"), translationPage);
        m_translationEnabledCheckBox->setChecked(m_translationSettings.enabled);
        translationLayout->addWidget(m_translationEnabledCheckBox);

        m_translationControls = new QWidget(translationPage);
        auto* translationControlsLayout = new QVBoxLayout(m_translationControls);
        translationControlsLayout->setContentsMargins(0, 0, 0, 0);
        auto* translationDescription = new QLabel(
            QStringLiteral("Translated message text is sent to Google Translate. Leave the API "
                           "key empty to use Javelin's built-in default key."),
            m_translationControls);
        translationDescription->setWordWrap(true);
        translationControlsLayout->addWidget(translationDescription);

        auto* translationForm = new QFormLayout();
        m_translationTargetLanguage = new QComboBox(m_translationControls);
        m_translationTargetLanguage->setEditable(true);
        m_translationTargetLanguage->addItem(QStringLiteral("English"), QStringLiteral("en"));
        m_translationTargetLanguage->addItem(QStringLiteral("Chinese (Simplified)"),
                                             QStringLiteral("zh-cn"));
        m_translationTargetLanguage->addItem(QStringLiteral("Chinese (Traditional)"),
                                             QStringLiteral("zh-tw"));
        m_translationTargetLanguage->addItem(QStringLiteral("French"), QStringLiteral("fr"));
        m_translationTargetLanguage->addItem(QStringLiteral("German"), QStringLiteral("de"));
        m_translationTargetLanguage->addItem(QStringLiteral("Italian"), QStringLiteral("it"));
        m_translationTargetLanguage->addItem(QStringLiteral("Japanese"), QStringLiteral("ja"));
        m_translationTargetLanguage->addItem(QStringLiteral("Korean"), QStringLiteral("ko"));
        m_translationTargetLanguage->addItem(QStringLiteral("Portuguese"), QStringLiteral("pt"));
        m_translationTargetLanguage->addItem(QStringLiteral("Russian"), QStringLiteral("ru"));
        m_translationTargetLanguage->addItem(QStringLiteral("Spanish"), QStringLiteral("es"));
        const int targetLanguageIndex =
            m_translationTargetLanguage->findData(m_translationSettings.targetLanguage);
        if (targetLanguageIndex >= 0)
        {
            m_translationTargetLanguage->setCurrentIndex(targetLanguageIndex);
        }
        else
        {
            m_translationTargetLanguage->setEditText(m_translationSettings.targetLanguage);
        }
        translationForm->addRow(QStringLiteral("Target language"), m_translationTargetLanguage);

        m_translationApiKeyEdit = new QLineEdit(m_translationControls);
        m_translationApiKeyEdit->setEchoMode(QLineEdit::Password);
        m_translationApiKeyEdit->setPlaceholderText(QStringLiteral("Use built-in default key"));
        m_translationApiKeyEdit->setText(m_translationSettings.apiKeyOverride);
        translationForm->addRow(QStringLiteral("API key override"), m_translationApiKeyEdit);
        translationControlsLayout->addLayout(translationForm);

        translationControlsLayout->addWidget(
            new QLabel(QStringLiteral("Auto-Translate Entries"), m_translationControls));
        m_autoTranslateList = new QListWidget(m_translationControls);
        m_autoTranslateList->setSelectionMode(QAbstractItemView::ExtendedSelection);
        translationControlsLayout->addWidget(m_autoTranslateList, 1);
        auto* translationButtons = new QHBoxLayout();
        translationButtons->addStretch(1);
        m_removeAutoTranslateButton =
            new QPushButton(QStringLiteral("Remove"), m_translationControls);
        translationButtons->addWidget(m_removeAutoTranslateButton);
        translationControlsLayout->addLayout(translationButtons);
        translationLayout->addWidget(m_translationControls, 1);
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

        auto* composingPage = new QWidget(this);
        auto* composingLayout = new QFormLayout(composingPage);
        m_undoSendDelaySpinBox = new QSpinBox(composingPage);
        m_undoSendDelaySpinBox->setRange(1, 120);
        m_undoSendDelaySpinBox->setSuffix(QStringLiteral(" seconds"));
        m_undoSendDelaySpinBox->setValue(m_undoSendDelaySeconds);
        composingLayout->addRow(QStringLiteral("Undo send window:"), m_undoSendDelaySpinBox);
        addPage(composingPage, QStringLiteral("Composing"), QStringLiteral("mail-send"), QString{},
                false);

        connect(addButton, &QPushButton::clicked, this, &PreferencesDialog::addAccount);
        connect(m_removeButton, &QPushButton::clicked, this,
                &PreferencesDialog::removeCurrentAccount);
        connect(m_accountList, &QListWidget::currentRowChanged, this,
                &PreferencesDialog::selectAccount);
        connect(m_displayNameEdit, &QLineEdit::textEdited, this,
                &PreferencesDialog::noteUnsavedChanges);
        connect(m_sessionUrlEdit, &QLineEdit::textEdited, this,
                &PreferencesDialog::noteConnectionSettingsChanged);
        connect(m_loginEmailEdit, &QLineEdit::textEdited, this,
                &PreferencesDialog::noteConnectionSettingsChanged);
        connect(m_apiKeyEdit, &QLineEdit::textEdited, this,
                &PreferencesDialog::noteConnectionSettingsChanged);
        connect(m_removeRemoteContentButton, &QPushButton::clicked, this,
                &PreferencesDialog::removeSelectedRemoteContentPermits);
        connect(m_remoteContentList, &QListWidget::itemSelectionChanged, this,
                [this]
                {
                    m_removeRemoteContentButton->setEnabled(
                        !m_remoteContentList->selectedItems().empty());
                });
        connect(m_messageColorMode, &QComboBox::currentIndexChanged, this,
                [this](const int index)
                {
                    m_messageAppearanceSettings.colorMode =
                        javelin::gui::messageview::messageColorModeFromStorage(
                            m_messageColorMode->itemData(index).toInt());
                    noteUnsavedChanges();
                });
        connect(m_translationEnabledCheckBox, &QCheckBox::toggled, this,
                [this](const bool enabled)
                {
                    m_translationSettings.enabled = enabled;
                    updateTranslationControls();
                    noteUnsavedChanges();
                });
        connect(m_translationTargetLanguage, &QComboBox::currentTextChanged, this,
                [this]
                {
                    m_translationSettings.targetLanguage =
                        selectedTranslationLanguageCode(*m_translationTargetLanguage);
                    noteUnsavedChanges();
                });
        connect(m_translationApiKeyEdit, &QLineEdit::textEdited, this,
                [this](const QString& apiKey)
                {
                    m_translationSettings.apiKeyOverride = apiKey;
                    noteUnsavedChanges();
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
        connect(m_undoSendDelaySpinBox, &QSpinBox::valueChanged, this,
                [this](const int seconds)
                {
                    m_undoSendDelaySeconds = seconds;
                    noteUnsavedChanges();
                });
        connect(m_mailboxSyncAccount, &QComboBox::currentIndexChanged, this,
                [this]
                {
                    storeMailboxSyncSelection();
                    m_mailboxSyncCurrentAccountId = m_mailboxSyncAccount->currentData().toString();
                    refreshMailboxSyncList();
                });
        connect(m_mailboxSyncModel, &QAbstractItemModel::dataChanged, this,
                [this]
                {
                    storeMailboxSyncSelection();
                    noteUnsavedChanges();
                });
        connect(m_mailboxNotificationModel, &QAbstractItemModel::dataChanged, this,
                [this]
                {
                    storeMailboxNotificationSelection();
                    noteUnsavedChanges();
                });

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
        refreshMailboxSyncAccounts();
        updateTranslationControls();
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

        if (saveCurrentSettings())
            KConfigDialog::updateSettings();
    }

    bool PreferencesDialog::hasChanged()
    {
        return m_hasPendingChanges;
    }

    void PreferencesDialog::selectConfiguredAccount(const QString& connectionId)
    {
        const auto found = std::ranges::find(m_accounts, connectionId, &ConnectionSettings::id);
        if (found == m_accounts.end())
            return;
        m_accountList->setCurrentRow(static_cast<int>(std::distance(m_accounts.begin(), found)));
    }

    void PreferencesDialog::addAccount()
    {
        storeCurrentEdits();
        if (m_hasPendingChanges && (!validateCurrentSettings() || !saveCurrentSettings()))
            return;

        QSet<QString> existingIds;
        for (const auto& account : m_settings.accounts())
            existingIds.insert(account.id);

        javelin::gui::onboarding::FirstRunWizard wizard{m_onboardingPort, m_settings, this};
        if (wizard.exec() != QDialog::Accepted)
            return;

        m_baseRevision = m_settings.snapshot().revision;
        m_accounts = m_settings.accounts();
        refreshAccountList();
        const auto added = std::ranges::find_if(m_accounts, [&existingIds](const auto& account)
                                                { return !existingIds.contains(account.id); });
        if (added == m_accounts.end())
            return;
        const auto row = static_cast<int>(std::distance(m_accounts.begin(), added));
        m_accountList->setCurrentRow(row);
        refreshMailboxSyncAccounts();
        Q_EMIT accountAdded(*added);
        QDialog::accept();
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

    void PreferencesDialog::noteConnectionSettingsChanged()
    {
        if (m_currentRow >= 0 && m_currentRow < static_cast<int>(m_accounts.size()))
            m_dirtyConnectionIds.insert(m_accounts[static_cast<std::size_t>(m_currentRow)].id);
        noteUnsavedChanges();
    }

    bool PreferencesDialog::saveCurrentSettings()
    {
        storeCurrentEdits();
        storeMailboxSyncSelection();
        for (auto& account : m_accounts)
        {
            if (m_dirtyConnectionIds.contains(account.id))
                ++account.revision;
        }

        m_remoteContentSenders.removeAll(QString{});
        m_remoteContentSenders.removeDuplicates();
        m_remoteContentSenders.sort(Qt::CaseInsensitive);
        m_remoteContentDomains.removeAll(QString{});
        m_remoteContentDomains.removeDuplicates();
        m_remoteContentDomains.sort(Qt::CaseInsensitive);
        m_translationSettings.enabled = m_translationEnabledCheckBox->isChecked();
        m_translationSettings.apiKeyOverride = m_translationApiKeyEdit->text();
        m_translationSettings.targetLanguage =
            selectedTranslationLanguageCode(*m_translationTargetLanguage);
        m_translationSettings.autoTranslateSenders = m_autoTranslateSenders;
        m_translationSettings.autoTranslateDomains = m_autoTranslateDomains;

        const auto selections =
            [](const QHash<QString, QStringList>& values, const QSet<QString>* configured)
        {
            std::vector<javelin::protocol::MailboxSelectionSettings> result;
            result.reserve(static_cast<std::size_t>(values.size()));
            for (auto it = values.cbegin(); it != values.cend(); ++it)
            {
                result.push_back({
                    .accountId = it.key(),
                    .mailboxIds = {it.value().begin(), it.value().end()},
                    .configured = configured == nullptr || configured->contains(it.key()),
                });
            }
            return result;
        };

        javelin::protocol::SettingsUpdate update;
        update.accounts = GuiSettings::protocolAccounts(m_accounts);
        update.syncedMailboxSelections = selections(m_syncedMailboxIds, nullptr);
        update.notificationMailboxSelections =
            selections(m_notificationMailboxIds, &m_configuredNotificationAccounts);
        update.remoteContentSenders =
            std::vector<QString>{m_remoteContentSenders.begin(), m_remoteContentSenders.end()};
        update.remoteContentDomains =
            std::vector<QString>{m_remoteContentDomains.begin(), m_remoteContentDomains.end()};
        update.translation = {
            .enabled = m_translationSettings.enabled,
            .apiKeyOverride = m_translationSettings.apiKeyOverride,
            .targetLanguage = m_translationSettings.targetLanguage,
            .autoTranslateSenders = {m_autoTranslateSenders.begin(), m_autoTranslateSenders.end()},
            .autoTranslateDomains = {m_autoTranslateDomains.begin(), m_autoTranslateDomains.end()},
        };
        update.appearance = {
            .messageColorMode = static_cast<std::int32_t>(m_messageAppearanceSettings.colorMode),
        };
        update.attachments = {
            .alwaysAsk = m_attachmentSaveSettings.alwaysAsk,
            .directory = m_attachmentSaveSettings.directory,
        };
        update.undoSendDelaySeconds = m_undoSendDelaySeconds;
        if (const auto error = m_settings.update(m_baseRevision, std::move(update)))
        {
            QMessageBox::critical(this, QStringLiteral("Could not save preferences"),
                                  error->detail);
            return false;
        }

        for (const auto& account : m_removedAccounts)
        {
            if (const auto error = m_accountCommandPort.removeConfiguredAccount(
                    account.loginEmail, account.sessionUrl, account.cachedAccountIds))
            {
                QMessageBox::critical(this, QStringLiteral("Could not remove account cache"),
                                      error->message);
            }
        }
        m_baseRevision = m_settings.snapshot().revision;
        m_removedAccounts.clear();
        m_dirtyConnectionIds.clear();
        m_loadedAccountIds.clear();
        for (const auto& account : m_accounts)
            m_loadedAccountIds.push_back(account.id);
        m_hasPendingChanges = false;
        return true;
    }

    void PreferencesDialog::refreshAccountList()
    {
        m_accountList->clear();
        for (const auto& account : m_accounts)
        {
            m_accountList->addItem(accountListText(account));
        }
    }

    void PreferencesDialog::refreshMailboxSyncAccounts()
    {
        QSignalBlocker blocker{m_mailboxSyncAccount};
        m_mailboxSyncAccount->clear();
        const auto accountsResult = m_accountReader.listAll();
        const auto* accounts =
            std::get_if<std::vector<javelin::jmap::cache::CachedAccount>>(&accountsResult);
        if (accounts == nullptr)
        {
            refreshMailboxSyncList();
            return;
        }
        for (const auto& account : *accounts)
        {
            const auto accountId = QString::fromStdString(account.accountId);
            const auto configuredAccount = m_settings.accountForCachedId(accountId);
            const auto accountName =
                !configuredAccount.displayName.isEmpty()
                    ? configuredAccount.displayName
                    : QString::fromStdString(account.name.empty() ? account.accountId
                                                                  : account.name);
            m_mailboxSyncAccount->addItem(accountName, accountId);
            m_syncedMailboxIds.insert(accountId, m_settings.syncedMailboxIds(accountId));
            m_notificationMailboxIds.insert(accountId,
                                            m_settings.notificationMailboxIds(accountId));
            if (m_settings.hasNotificationMailboxSelection(accountId))
                m_configuredNotificationAccounts.insert(accountId);
        }
        m_mailboxSyncCurrentAccountId = m_mailboxSyncAccount->currentData().toString();
        refreshMailboxSyncList();
    }

    void PreferencesDialog::refreshMailboxSyncList()
    {
        QSignalBlocker blocker{m_mailboxSyncModel};
        QSignalBlocker notificationBlocker{m_mailboxNotificationModel};
        const auto accountId = m_mailboxSyncCurrentAccountId;
        if (accountId.isEmpty())
        {
            m_mailboxSyncModel->setAccountId(std::string{});
            m_mailboxNotificationModel->setAccountId(std::string{});
            return;
        }
        const auto result = m_mailboxReader.listMailboxTree(accountId.toStdString());
        const auto* mailboxes =
            std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&result);
        if (mailboxes == nullptr)
        {
            return;
        }
        const auto selected = m_syncedMailboxIds.value(accountId);
        auto notificationSelected = m_notificationMailboxIds.value(accountId);
        if (!m_configuredNotificationAccounts.contains(accountId))
        {
            const auto inbox = std::ranges::find(*mailboxes, std::optional<std::string>{"inbox"},
                                                 &javelin::jmap::cache::MailboxTreeItem::role);
            if (inbox != mailboxes->end())
            {
                notificationSelected.push_back(QString::fromStdString(inbox->id));
                m_notificationMailboxIds.insert(accountId, notificationSelected);
            }
        }
        const auto modelAccountId = std::optional<std::string>{accountId.toStdString()};
        m_mailboxSyncModel->setCheckedMailboxIds(selected);
        m_mailboxSyncModel->setAccountId(modelAccountId);
        m_mailboxNotificationModel->setCheckedMailboxIds(notificationSelected);
        m_mailboxNotificationModel->setAccountId(modelAccountId);
        m_mailboxSyncList->expandAll();
        m_mailboxNotificationList->expandAll();
    }

    void PreferencesDialog::storeMailboxNotificationSelection()
    {
        const auto accountId = m_mailboxSyncCurrentAccountId;
        if (accountId.isEmpty())
        {
            return;
        }
        const auto selected = m_mailboxNotificationModel->checkedMailboxIds();
        m_notificationMailboxIds.insert(accountId, selected);
        m_configuredNotificationAccounts.insert(accountId);
    }

    void PreferencesDialog::storeMailboxSyncSelection()
    {
        const auto accountId = m_mailboxSyncCurrentAccountId;
        if (accountId.isEmpty())
        {
            return;
        }
        const auto selected = m_mailboxSyncModel->checkedMailboxIds();
        m_syncedMailboxIds.insert(accountId, selected);
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

    void PreferencesDialog::updateTranslationControls()
    {
        m_translationControls->setEnabled(m_translationEnabledCheckBox->isChecked());
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
        if (m_translationEnabledCheckBox->isChecked() &&
            selectedTranslationLanguageCode(*m_translationTargetLanguage).isEmpty())
        {
            QMessageBox::warning(this, QStringLiteral("Invalid translation language"),
                                 QStringLiteral("Choose or enter a target language code."));
            m_translationTargetLanguage->setFocus();
            return false;
        }

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
