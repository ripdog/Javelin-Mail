#include "gui/settings/PreferencesDialog.h"

#include "app/AccountApplicationPorts.h"
#include "gui/mailboxes/MailboxTreeModel.h"
#include "gui/mailboxes/MailboxTreeView.h"
#include "gui/onboarding/FirstRunWizard.h"
#include "gui/translation/TranslationService.h"
#include "jmap/cache/AccountReadRepository.h"
#include "jmap/cache/MailboxReadRepository.h"

#include <QCoroTask>

#include <KLocalizedString>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QSplitter>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>
#include <utility>

namespace javelin::gui::settings
{
    namespace
    {
        constexpr int remoteContentKindRole = Qt::UserRole + 1;
        constexpr int remoteContentValueRole = Qt::UserRole + 2;
        constexpr int autoTranslateKindRole = Qt::UserRole + 1;
        constexpr int autoTranslateValueRole = Qt::UserRole + 2;
        constexpr int localModelSourceRole = Qt::UserRole + 1;
        constexpr int localModelTargetRole = Qt::UserRole + 2;

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
            QString text;
            if (!account.displayName.isEmpty() && !account.loginEmail.isEmpty() &&
                account.displayName.compare(account.loginEmail, Qt::CaseInsensitive) != 0)
            {
                text = QStringLiteral("%1 — %2").arg(account.displayName, account.loginEmail);
            }
            else if (!account.displayName.isEmpty())
            {
                text = account.displayName;
            }
            else
            {
                text = account.loginEmail.isEmpty() ? i18n("New account") : account.loginEmail;
            }
            return account.reauthenticationRequired ? i18n("%1 — Sign-in required", text) : text;
        }

        [[nodiscard]] javelin::gui::translation::TranslationProvider
        selectedTranslationProvider(const QComboBox& comboBox)
        {
            return static_cast<javelin::gui::translation::TranslationProvider>(
                comboBox.currentData().toInt());
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

        [[nodiscard]] QString languageDisplayName(const QStringView languageCode)
        {
            const QLocale locale{languageCode.toString()};
            const auto name = QLocale::languageToString(locale.language());
            return locale.language() == QLocale::C ? languageCode.toString() : name;
        }

    } // namespace

    PreferencesDialog::PreferencesDialog(
        GuiSettings& settings, javelin::app::AccountCommandPort& accountCommandPort,
        javelin::app::OnboardingPort& onboardingPort,
        javelin::gui::translation::TranslationService& translationService,
        javelin::jmap::cache::AccountReader& accountReader,
        javelin::jmap::cache::MailboxReader& mailboxReader, QWidget* parent)
        : KConfigDialog(parent, QStringLiteral("preferences"), nullptr), m_settings(settings),
          m_baseRevision(m_settings.snapshot().revision), m_accountCommandPort(accountCommandPort),
          m_onboardingPort(onboardingPort), m_translationService(translationService),
          m_accountReader(accountReader), m_mailboxReader(mailboxReader),
          m_accounts(m_settings.accounts()),
          m_remoteContentSenders(m_settings.remoteContentSenders()),
          m_remoteContentDomains(m_settings.remoteContentDomains()),
          m_translationSettings(m_translationService.settings()),
          m_autoTranslateSenders(m_translationSettings.autoTranslateSenders),
          m_autoTranslateDomains(m_translationSettings.autoTranslateDomains),
          m_messageAppearanceSettings(m_settings.messageAppearanceSettings()),
          m_attachmentSaveSettings(m_settings.attachmentSaveSettings()),
          m_undoSendDelaySeconds(m_settings.undoSendDelaySeconds())
    {
        setWindowTitle(i18n("Preferences"));
        resize(760, 420);

        auto* accountsPage = new QWidget(this);
        auto* accountsPageLayout = new QVBoxLayout(accountsPage);
        auto* splitter = new QSplitter(accountsPage);
        auto* accountPanel = new QWidget(splitter);
        auto* accountLayout = new QVBoxLayout(accountPanel);
        accountLayout->addWidget(new QLabel(i18n("Accounts"), accountPanel));
        m_accountList = new QListWidget(accountPanel);
        accountLayout->addWidget(m_accountList, 1);

        auto* accountButtons = new QHBoxLayout();
        auto* addButton = new QPushButton(i18nc("@action:button", "Add"), accountPanel);
        m_removeButton = new QPushButton(i18nc("@action:button", "Remove"), accountPanel);
        accountButtons->addWidget(addButton);
        accountButtons->addWidget(m_removeButton);
        accountLayout->addLayout(accountButtons);

        auto* detailsPanel = new QWidget(splitter);
        auto* detailsLayout = new QVBoxLayout(detailsPanel);
        auto* formLayout = new QFormLayout();
        m_displayNameEdit = new QLineEdit(detailsPanel);
        m_displayNameEdit->setPlaceholderText(i18nc("@info:placeholder account name", "Personal"));
        m_loginEmailLabel = new QLabel(detailsPanel);
        m_loginEmailLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_sessionUrlLabel = new QLabel(detailsPanel);
        m_sessionUrlLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_sessionUrlLabel->setWordWrap(true);
        formLayout->addRow(i18n("Display Name"), m_displayNameEdit);
        formLayout->addRow(i18n("Email"), m_loginEmailLabel);
        formLayout->addRow(i18n("Mail Server"), m_sessionUrlLabel);
        detailsLayout->addLayout(formLayout);
        auto* managedDetails =
            new QLabel(i18n("Sign-in and server details are managed automatically."), detailsPanel);
        managedDetails->setWordWrap(true);
        managedDetails->setForegroundRole(QPalette::PlaceholderText);
        detailsLayout->addWidget(managedDetails);
        m_reauthenticateButton = new QPushButton(i18n("Sign In Again"), detailsPanel);
        detailsLayout->addWidget(m_reauthenticateButton);
        detailsLayout->addStretch();

        splitter->addWidget(accountPanel);
        splitter->addWidget(detailsPanel);
        splitter->setStretchFactor(1, 1);
        accountsPageLayout->addWidget(splitter, 1);
        addPage(accountsPage, i18n("Accounts"), QStringLiteral("user-identity"), QString{}, false);

        auto* mailboxSyncPage = new QWidget(this);
        auto* mailboxSyncLayout = new QVBoxLayout(mailboxSyncPage);
        auto* mailboxSyncDescription = new QLabel(
            i18n("Download every message and attachment in selected mailboxes for complete "
                 "offline access. Large mailboxes continue in the Task Center and can be paused. "
                 "Unchecking keeps downloaded mail as removable cache."),
            mailboxSyncPage);
        mailboxSyncDescription->setWordWrap(true);
        mailboxSyncDescription->setSizePolicy(
            QSizePolicy{QSizePolicy::Preferred, QSizePolicy::Minimum});
        mailboxSyncLayout->addWidget(mailboxSyncDescription);
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
        auto* mailboxLists = new QGridLayout();
        auto* syncListLayout = new QVBoxLayout();
        syncListLayout->addWidget(new QLabel(i18n("Keep complete offline copy"), mailboxSyncPage));
        m_mailboxSyncList->setSizePolicy(QSizePolicy{QSizePolicy::Ignored, QSizePolicy::Expanding});
        syncListLayout->addWidget(m_mailboxSyncList, 1);
        mailboxLists->addLayout(syncListLayout, 0, 0);
        auto* notificationListLayout = new QVBoxLayout();
        notificationListLayout->addWidget(new QLabel(i18n("Show notifications"), mailboxSyncPage));
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
        m_mailboxNotificationList->setSizePolicy(
            QSizePolicy{QSizePolicy::Ignored, QSizePolicy::Expanding});
        notificationListLayout->addWidget(m_mailboxNotificationList, 1);
        mailboxLists->addLayout(notificationListLayout, 0, 1);
        mailboxLists->setColumnStretch(0, 1);
        mailboxLists->setColumnStretch(1, 1);
        mailboxSyncLayout->addLayout(mailboxLists, 1);
        addPage(mailboxSyncPage, i18n("Mailbox Sync"), QStringLiteral("view-refresh"), QString{},
                false);

        auto* remoteContentPage = new QWidget(this);
        auto* remoteContentLayout = new QVBoxLayout(remoteContentPage);
        remoteContentLayout->addWidget(
            new QLabel(i18n("Allowed Remote Content"), remoteContentPage));
        m_remoteContentList = new QListWidget(remoteContentPage);
        m_remoteContentList->setSelectionMode(QAbstractItemView::ExtendedSelection);
        remoteContentLayout->addWidget(m_remoteContentList, 1);
        auto* remoteContentButtons = new QHBoxLayout();
        remoteContentButtons->addStretch(1);
        m_removeRemoteContentButton =
            new QPushButton(i18nc("@action:button", "Remove"), remoteContentPage);
        remoteContentButtons->addWidget(m_removeRemoteContentButton);
        remoteContentLayout->addLayout(remoteContentButtons);
        addPage(remoteContentPage, i18n("Remote Content"), QStringLiteral("network-wireless-on"),
                QString{}, false);

        auto* appearancePage = new QWidget(this);
        auto* appearanceLayout = new QFormLayout(appearancePage);
        m_messageColorMode = new QComboBox(appearancePage);
        m_messageColorMode->addItem(
            i18n("Follow application"),
            static_cast<int>(javelin::gui::messageview::MessageColorMode::FollowApplication));
        m_messageColorMode->addItem(
            i18n("Always use original colours"),
            static_cast<int>(javelin::gui::messageview::MessageColorMode::Light));
        m_messageColorMode->addItem(
            i18n("Always use dark colours"),
            static_cast<int>(javelin::gui::messageview::MessageColorMode::Dark));
        const int messageColorModeIndex =
            m_messageColorMode->findData(static_cast<int>(m_messageAppearanceSettings.colorMode));
        m_messageColorMode->setCurrentIndex(messageColorModeIndex);
        appearanceLayout->addRow(i18n("HTML message colours"), m_messageColorMode);
        addPage(appearancePage, i18n("Appearance"), QStringLiteral("preferences-desktop-theme"),
                QString{}, false);

        auto* translationPage = new QWidget(this);
        auto* translationLayout = new QVBoxLayout(translationPage);
        auto* providerForm = new QFormLayout();
        m_translationProvider = new QComboBox(translationPage);
        m_translationProvider->addItem(
            i18nc("@item translation provider", "Disabled"),
            static_cast<int>(javelin::gui::translation::TranslationProvider::Disabled));
        m_translationProvider->addItem(
            i18nc("@item translation provider", "Google Translate"),
            static_cast<int>(javelin::gui::translation::TranslationProvider::Google));
        if (m_translationService.localProviderAvailable())
        {
            m_translationProvider->addItem(
                i18nc("@item translation provider", "Local (Firefox models)"),
                static_cast<int>(javelin::gui::translation::TranslationProvider::Local));
        }
        const auto providerIndex =
            m_translationProvider->findData(static_cast<int>(m_translationSettings.provider));
        m_translationProvider->setCurrentIndex(providerIndex >= 0 ? providerIndex : 1);
        providerForm->addRow(i18n("Translation provider"), m_translationProvider);
        translationLayout->addLayout(providerForm);

        m_translationControls = new QWidget(translationPage);
        auto* translationControlsLayout = new QVBoxLayout(m_translationControls);
        translationControlsLayout->setContentsMargins(0, 0, 0, 0);
        auto* translationForm = new QFormLayout();
        m_translationTargetLanguage = new QComboBox(m_translationControls);
        m_translationTargetLanguage->setEditable(true);
        m_translationTargetLanguage->addItem(i18n("English"), QStringLiteral("en"));
        m_translationTargetLanguage->addItem(i18n("Chinese (Simplified)"),
                                             QStringLiteral("zh-Hans"));
        m_translationTargetLanguage->addItem(i18n("Chinese (Traditional)"),
                                             QStringLiteral("zh-Hant"));
        m_translationTargetLanguage->addItem(i18n("French"), QStringLiteral("fr"));
        m_translationTargetLanguage->addItem(i18n("German"), QStringLiteral("de"));
        m_translationTargetLanguage->addItem(i18n("Italian"), QStringLiteral("it"));
        m_translationTargetLanguage->addItem(i18n("Japanese"), QStringLiteral("ja"));
        m_translationTargetLanguage->addItem(i18n("Korean"), QStringLiteral("ko"));
        m_translationTargetLanguage->addItem(i18n("Portuguese"), QStringLiteral("pt"));
        m_translationTargetLanguage->addItem(i18n("Russian"), QStringLiteral("ru"));
        m_translationTargetLanguage->addItem(i18n("Spanish"), QStringLiteral("es"));
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
        translationForm->addRow(i18n("Target language"), m_translationTargetLanguage);

        translationControlsLayout->addLayout(translationForm);

        m_googleTranslationControls = new QWidget(m_translationControls);
        auto* googleLayout = new QVBoxLayout(m_googleTranslationControls);
        googleLayout->setContentsMargins(0, 0, 0, 0);
        auto* googleDescription = new QLabel(
            i18n("Translated message text is sent to Google Translate. Leave the API key empty "
                 "to use Javelin's built-in default key."),
            m_googleTranslationControls);
        googleDescription->setWordWrap(true);
        googleLayout->addWidget(googleDescription);
        auto* googleForm = new QFormLayout();
        m_translationApiKeyEdit = new QLineEdit(m_googleTranslationControls);
        m_translationApiKeyEdit->setEchoMode(QLineEdit::Password);
        m_translationApiKeyEdit->setPlaceholderText(i18n("Use built-in default key"));
        m_translationApiKeyEdit->setText(m_translationSettings.apiKeyOverride);
        googleForm->addRow(i18n("API key override"), m_translationApiKeyEdit);
        googleLayout->addLayout(googleForm);
        translationControlsLayout->addWidget(m_googleTranslationControls);

        m_localTranslationControls = new QWidget(m_translationControls);
        auto* localLayout = new QVBoxLayout(m_localTranslationControls);
        localLayout->setContentsMargins(0, 0, 0, 0);
        auto* localDescription = new QLabel(
            i18n("Local translation runs only in the Javelin GUI. Firefox-compatible models are "
                 "downloaded on demand and remain on this computer."),
            m_localTranslationControls);
        localDescription->setWordWrap(true);
        localLayout->addWidget(localDescription);
        auto* localRouteLayout = new QHBoxLayout();
        m_localTranslationSource = new QComboBox(m_localTranslationControls);
        m_localTranslationTarget = new QComboBox(m_localTranslationControls);
        for (const auto& language : m_translationService.localSourceLanguages())
        {
            m_localTranslationSource->addItem(language, language);
        }
        localRouteLayout->addWidget(new QLabel(i18nc("@label translation source language", "From"),
                                               m_localTranslationControls));
        localRouteLayout->addWidget(m_localTranslationSource, 1);
        localRouteLayout->addWidget(new QLabel(i18nc("@label translation target language", "to"),
                                               m_localTranslationControls));
        localRouteLayout->addWidget(m_localTranslationTarget, 1);
        m_downloadLocalModelsButton =
            new QPushButton(i18n("Download models"), m_localTranslationControls);
        localRouteLayout->addWidget(m_downloadLocalModelsButton);
        localLayout->addLayout(localRouteLayout);
        m_localModelStatus = new QLabel(m_localTranslationControls);
        m_localModelStatus->setWordWrap(true);
        localLayout->addWidget(m_localModelStatus);
        localLayout->addWidget(
            new QLabel(i18n("Downloaded model directions"), m_localTranslationControls));
        m_installedLocalModels = new QListWidget(m_localTranslationControls);
        m_installedLocalModels->setSelectionMode(QAbstractItemView::ExtendedSelection);
        localLayout->addWidget(m_installedLocalModels);
        auto* localButtons = new QHBoxLayout();
        localButtons->addStretch(1);
        m_removeLocalModelsButton =
            new QPushButton(i18nc("@action:button", "Remove"), m_localTranslationControls);
        localButtons->addWidget(m_removeLocalModelsButton);
        localLayout->addLayout(localButtons);
        translationControlsLayout->addWidget(m_localTranslationControls);

        translationControlsLayout->addWidget(
            new QLabel(i18n("Auto-Translate Entries"), m_translationControls));
        m_autoTranslateList = new QListWidget(m_translationControls);
        m_autoTranslateList->setSelectionMode(QAbstractItemView::ExtendedSelection);
        translationControlsLayout->addWidget(m_autoTranslateList, 1);
        auto* translationButtons = new QHBoxLayout();
        translationButtons->addStretch(1);
        m_removeAutoTranslateButton =
            new QPushButton(i18nc("@action:button", "Remove"), m_translationControls);
        translationButtons->addWidget(m_removeAutoTranslateButton);
        translationControlsLayout->addLayout(translationButtons);
        translationLayout->addWidget(m_translationControls, 1);
        addPage(translationPage, i18n("Translation"), QStringLiteral("preferences-desktop-locale"),
                QString{}, false);

        auto* attachmentsPage = new QWidget(this);
        auto* attachmentsLayout = new QVBoxLayout(attachmentsPage);
        m_askAttachmentDirectoryRadio =
            new QRadioButton(i18n("Always ask where to save attachments"), attachmentsPage);
        m_saveAttachmentDirectoryRadio =
            new QRadioButton(i18n("Always save attachments to:"), attachmentsPage);
        attachmentsLayout->addWidget(m_askAttachmentDirectoryRadio);
        attachmentsLayout->addWidget(m_saveAttachmentDirectoryRadio);
        auto* attachmentDirectoryLayout = new QHBoxLayout();
        m_attachmentDirectoryEdit = new QLineEdit(attachmentsPage);
        m_attachmentDirectoryEdit->setReadOnly(true);
        m_attachmentDirectoryButton = new QPushButton(i18n("Choose..."), attachmentsPage);
        attachmentDirectoryLayout->addWidget(m_attachmentDirectoryEdit, 1);
        attachmentDirectoryLayout->addWidget(m_attachmentDirectoryButton);
        attachmentsLayout->addLayout(attachmentDirectoryLayout);
        attachmentsLayout->addStretch(1);
        m_askAttachmentDirectoryRadio->setChecked(m_attachmentSaveSettings.alwaysAsk);
        m_saveAttachmentDirectoryRadio->setChecked(!m_attachmentSaveSettings.alwaysAsk);
        m_attachmentDirectoryEdit->setText(m_attachmentSaveSettings.directory);
        addPage(attachmentsPage, i18n("Attachments"), QStringLiteral("mail-attachment"), QString{},
                false);

        auto* composingPage = new QWidget(this);
        auto* composingLayout = new QFormLayout(composingPage);
        m_undoSendDelaySpinBox = new QSpinBox(composingPage);
        m_undoSendDelaySpinBox->setRange(1, 120);
        m_undoSendDelaySpinBox->setSuffix(i18nc("@item time suffix", " seconds"));
        m_undoSendDelaySpinBox->setValue(m_undoSendDelaySeconds);
        composingLayout->addRow(i18n("Undo send window:"), m_undoSendDelaySpinBox);
        addPage(composingPage, i18n("Composing"), QStringLiteral("mail-send"), QString{}, false);

        connect(addButton, &QPushButton::clicked, this, &PreferencesDialog::addAccount);
        connect(m_reauthenticateButton, &QPushButton::clicked, this,
                &PreferencesDialog::reauthenticateCurrentAccount);
        connect(m_removeButton, &QPushButton::clicked, this,
                &PreferencesDialog::removeCurrentAccount);
        connect(m_accountList, &QListWidget::currentRowChanged, this,
                &PreferencesDialog::selectAccount);
        connect(m_displayNameEdit, &QLineEdit::textEdited, this,
                &PreferencesDialog::noteUnsavedChanges);
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
        connect(m_translationProvider, &QComboBox::currentIndexChanged, this,
                [this]
                {
                    m_translationSettings.provider =
                        selectedTranslationProvider(*m_translationProvider);
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
        connect(m_localTranslationSource, &QComboBox::currentIndexChanged, this,
                &PreferencesDialog::updateLocalTranslationTargets);
        connect(m_downloadLocalModelsButton, &QPushButton::clicked, this,
                &PreferencesDialog::downloadSelectedLocalModels);
        connect(m_removeLocalModelsButton, &QPushButton::clicked, this,
                &PreferencesDialog::removeSelectedLocalModels);
        connect(m_installedLocalModels, &QListWidget::itemSelectionChanged, this,
                [this]
                {
                    m_removeLocalModelsButton->setEnabled(
                        !m_installedLocalModels->selectedItems().empty());
                });
        connect(&m_translationService,
                &javelin::gui::translation::TranslationService::localModelDownloadProgress, this,
                [this](const QString& sourceLanguage, const QString& targetLanguage,
                       const qint64 received, const qint64 total)
                {
                    m_localModelStatus->setText(i18n(
                        "Downloading %1 → %2: %3 of %4", languageDisplayName(sourceLanguage),
                        languageDisplayName(targetLanguage), QLocale{}.formattedDataSize(received),
                        QLocale{}.formattedDataSize(total)));
                });
        connect(&m_translationService,
                &javelin::gui::translation::TranslationService::installedLocalModelsChanged, this,
                &PreferencesDialog::refreshInstalledLocalModels);
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
        updateLocalTranslationTargets();
        refreshInstalledLocalModels();
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
        QMessageBox warning{
            QMessageBox::Warning, i18n("Remove account"),
            i18n("Applying this change will permanently wipe all of this account's cached mail, "
                 "drafts, state, and pending actions."),
            QMessageBox::Cancel, this};
        auto* removeButton =
            warning.addButton(i18n("Remove Account"), QMessageBox::DestructiveRole);
        auto* confirmation =
            new QCheckBox(i18n("I understand that all cached data will be wiped."), &warning);
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

    void PreferencesDialog::reauthenticateCurrentAccount()
    {
        storeCurrentEdits();
        if (m_currentRow < 0 || m_currentRow >= static_cast<int>(m_accounts.size()))
            return;
        if (m_hasPendingChanges && (!validateCurrentSettings() || !saveCurrentSettings()))
            return;

        const auto connectionId = m_accounts[static_cast<std::size_t>(m_currentRow)].id;
        javelin::gui::onboarding::FirstRunWizard wizard{m_onboardingPort, m_settings, connectionId,
                                                        this};
        if (wizard.exec() != QDialog::Accepted)
            return;

        m_baseRevision = m_settings.snapshot().revision;
        m_accounts = m_settings.accounts();
        refreshAccountList();
        const auto account = std::ranges::find(m_accounts, connectionId, &ConnectionSettings::id);
        if (account == m_accounts.end())
            return;
        m_accountList->setCurrentRow(static_cast<int>(std::distance(m_accounts.begin(), account)));
        refreshMailboxSyncAccounts();
        Q_EMIT accountReauthenticated(*account);
        QDialog::accept();
    }

    void PreferencesDialog::selectAccount(const int row)
    {
        storeCurrentEdits();
        m_currentRow = row;
        const bool hasAccount = row >= 0 && row < static_cast<int>(m_accounts.size());
        m_removeButton->setEnabled(hasAccount);
        m_reauthenticateButton->setEnabled(hasAccount);
        m_displayNameEdit->setEnabled(hasAccount);
        if (!hasAccount)
        {
            m_displayNameEdit->clear();
            m_loginEmailLabel->clear();
            m_sessionUrlLabel->clear();
            return;
        }

        const auto& account = m_accounts[static_cast<std::size_t>(row)];
        m_displayNameEdit->setText(account.displayName);
        m_loginEmailLabel->setText(account.loginEmail);
        m_sessionUrlLabel->setText(account.sessionUrl);
    }

    void PreferencesDialog::storeCurrentEdits()
    {
        if (m_currentRow < 0 || m_currentRow >= static_cast<int>(m_accounts.size()))
        {
            return;
        }
        auto& account = m_accounts[static_cast<std::size_t>(m_currentRow)];
        account.displayName = m_displayNameEdit->text().trimmed();
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

    bool PreferencesDialog::saveCurrentSettings()
    {
        storeCurrentEdits();
        storeMailboxSyncSelection();

        m_remoteContentSenders.removeAll(QString{});
        m_remoteContentSenders.removeDuplicates();
        m_remoteContentSenders.sort(Qt::CaseInsensitive);
        m_remoteContentDomains.removeAll(QString{});
        m_remoteContentDomains.removeDuplicates();
        m_remoteContentDomains.sort(Qt::CaseInsensitive);
        m_translationSettings.provider = selectedTranslationProvider(*m_translationProvider);
        m_translationSettings.apiKeyOverride = m_translationApiKeyEdit->text();
        m_translationSettings.targetLanguage =
            selectedTranslationLanguageCode(*m_translationTargetLanguage);
        m_translationSettings.autoTranslateSenders = m_autoTranslateSenders;
        m_translationSettings.autoTranslateDomains = m_autoTranslateDomains;

        const auto selections = [](const QHash<QString, QStringList>& values)
        {
            std::vector<javelin::protocol::MailboxSelectionSettings> result;
            result.reserve(static_cast<std::size_t>(values.size()));
            for (auto it = values.cbegin(); it != values.cend(); ++it)
            {
                result.push_back({
                    .accountId = it.key(),
                    .mailboxIds = {it.value().begin(), it.value().end()},
                });
            }
            return result;
        };

        javelin::protocol::SettingsUpdate update;
        update.accounts = GuiSettings::protocolAccounts(m_accounts);
        update.syncedMailboxSelections = selections(m_syncedMailboxIds);
        update.notificationMailboxSelections = selections(m_notificationMailboxIds);
        update.remoteContentSenders =
            std::vector<QString>{m_remoteContentSenders.begin(), m_remoteContentSenders.end()};
        update.remoteContentDomains =
            std::vector<QString>{m_remoteContentDomains.begin(), m_remoteContentDomains.end()};
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
            QMessageBox::critical(this, i18n("Could not save preferences"), error->detail);
            return false;
        }
        m_baseRevision = m_settings.snapshot().revision;
        if (const auto error = m_translationService.saveSettings(m_translationSettings))
        {
            QMessageBox::critical(this, i18n("Could not save translation preferences"),
                                  error->message);
            return false;
        }

        for (const auto& account : m_removedAccounts)
        {
            if (const auto error = m_accountCommandPort.removeConfiguredAccount(
                    account.loginEmail, account.sessionUrl, account.cachedAccountIds))
            {
                QMessageBox::critical(this, i18n("Could not remove account cache"), error->message);
            }
        }
        m_baseRevision = m_settings.snapshot().revision;
        m_removedAccounts.clear();
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
        const auto notificationSelected = m_notificationMailboxIds.value(accountId);
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
                new QListWidgetItem(i18nc("@item remote-content permission", "Sender: %1", sender),
                                    m_remoteContentList);
            item->setData(remoteContentKindRole, static_cast<int>(RemoteContentPermitKind::Sender));
            item->setData(remoteContentValueRole, sender);
        }

        for (const auto& domain : m_remoteContentDomains)
        {
            auto* item =
                new QListWidgetItem(i18nc("@item remote-content permission", "Domain: %1", domain),
                                    m_remoteContentList);
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
                new QListWidgetItem(i18nc("@item automatic translation rule", "Sender: %1", sender),
                                    m_autoTranslateList);
            item->setData(autoTranslateKindRole, static_cast<int>(AutoTranslateEntryKind::Sender));
            item->setData(autoTranslateValueRole, sender);
        }

        for (const auto& domain : m_autoTranslateDomains)
        {
            auto* item =
                new QListWidgetItem(i18nc("@item automatic translation rule", "Domain: %1", domain),
                                    m_autoTranslateList);
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
        const auto provider = selectedTranslationProvider(*m_translationProvider);
        const bool enabled = provider != javelin::gui::translation::TranslationProvider::Disabled;
        m_translationControls->setEnabled(enabled);
        m_googleTranslationControls->setVisible(
            provider == javelin::gui::translation::TranslationProvider::Google);
        m_localTranslationControls->setVisible(
            provider == javelin::gui::translation::TranslationProvider::Local);
    }

    void PreferencesDialog::updateLocalTranslationTargets()
    {
        const auto source = m_localTranslationSource->currentData().toString();
        const auto previousTarget = m_localTranslationTarget->currentData().toString();
        const QSignalBlocker blocker{m_localTranslationTarget};
        m_localTranslationTarget->clear();
        for (const auto& target : m_translationService.localTargetLanguages(source))
        {
            m_localTranslationTarget->addItem(target, target);
        }
        const auto previousIndex = m_localTranslationTarget->findData(previousTarget);
        if (previousIndex >= 0)
        {
            m_localTranslationTarget->setCurrentIndex(previousIndex);
        }
        m_downloadLocalModelsButton->setEnabled(!source.isEmpty() &&
                                                m_localTranslationTarget->count() > 0);
    }

    void PreferencesDialog::refreshInstalledLocalModels()
    {
        m_installedLocalModels->clear();
        qint64 totalSize = 0;
        for (const auto& model : m_translationService.installedLocalModels())
        {
            totalSize += model.diskSize;
            auto* item = new QListWidgetItem(
                i18nc("@item installed translation model", "%1 → %2 — %3 (%4), %5",
                      model.sourceLanguage, model.targetLanguage, model.modelVersion,
                      model.architecture, QLocale{}.formattedDataSize(model.diskSize)),
                m_installedLocalModels);
            item->setData(localModelSourceRole, model.sourceLanguage);
            item->setData(localModelTargetRole, model.targetLanguage);
        }
        m_removeLocalModelsButton->setEnabled(false);
        if (m_installedLocalModels->count() == 0)
        {
            m_localModelStatus->setText(i18n("No local translation models are installed."));
        }
        else
        {
            m_localModelStatus->setText(i18np("%1 model direction installed, using %2.",
                                              "%1 model directions installed, using %2.",
                                              m_installedLocalModels->count(),
                                              QLocale{}.formattedDataSize(totalSize)));
        }
    }

    void PreferencesDialog::downloadSelectedLocalModels()
    {
        const auto source = m_localTranslationSource->currentData().toString();
        const auto target = m_localTranslationTarget->currentData().toString();
        if (source.isEmpty() || target.isEmpty())
        {
            return;
        }
        m_downloadLocalModelsButton->setEnabled(false);
        m_localModelStatus->setText(i18n("Preparing %1 → %2 models…", source, target));
        auto task = m_translationService.installLocalModels(source, target);
        QCoro::connect(std::move(task), this,
                       [this](std::optional<javelin::gui::translation::TranslationError> error)
                       {
                           m_downloadLocalModelsButton->setEnabled(true);
                           if (error.has_value())
                           {
                               m_localModelStatus->setText(
                                   i18n("Model download failed: %1", error->message));
                               return;
                           }
                           refreshInstalledLocalModels();
                       });
    }

    void PreferencesDialog::removeSelectedLocalModels()
    {
        for (const auto* item : m_installedLocalModels->selectedItems())
        {
            const auto source = item->data(localModelSourceRole).toString();
            const auto target = item->data(localModelTargetRole).toString();
            if (const auto error = m_translationService.removeLocalModels(source, target))
            {
                QMessageBox::critical(this, i18n("Could not remove translation model"),
                                      error->message);
                return;
            }
        }
        refreshInstalledLocalModels();
    }

    void PreferencesDialog::selectAttachmentDirectory()
    {
        const auto directory = QFileDialog::getExistingDirectory(
            this, i18n("Select Attachment Directory"), m_attachmentSaveSettings.directory);
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
        if (selectedTranslationProvider(*m_translationProvider) !=
                javelin::gui::translation::TranslationProvider::Disabled &&
            selectedTranslationLanguageCode(*m_translationTargetLanguage).isEmpty())
        {
            QMessageBox::warning(this, i18n("Invalid translation language"),
                                 i18n("Choose or enter a target language code."));
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

        QMessageBox::warning(
            this, i18n("Invalid attachment directory"),
            i18n("Choose an existing directory for attachments, or select always asking where to "
                 "save attachments."));
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
