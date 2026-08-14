#include "gui/settings/PreferencesDialog.h"
#include "gui/settings/PreferencesPages.h"

#include "app/AccountApplicationPorts.h"
#include "app/DeveloperDiagnostics.h"
#include "app/DeveloperMaintenance.h"
#include "app/MailApplicationPorts.h"
#include "app/OnboardingApplicationPorts.h"
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
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QSplitter>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>
#include <utility>
#include <variant>

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

        struct MailboxCacheClearRequest
        {
            QString accountId;
            QString mailboxId;
            bool clearSqlite = false;
            bool clearBodies = false;
        };

        struct MailboxVisibilityChangeRequest
        {
            std::string accountId;
            std::string mailboxId;
            bool subscribed = true;
            bool previousOffline = false;
            bool previousNotifications = false;
        };

        struct MailboxVisibilityChangeFailure
        {
            MailboxVisibilityChangeRequest change;
            javelin::jmap::OperationError error;
        };

        [[nodiscard]] bool
        mailboxSelected(const std::vector<javelin::protocol::MailboxSelectionSettings>& selections,
                        const QStringView accountId, const QStringView mailboxId)
        {
            const auto account =
                std::ranges::find(selections, accountId.toString(),
                                  &javelin::protocol::MailboxSelectionSettings::accountId);
            return account != selections.end() &&
                   std::ranges::contains(account->mailboxIds, mailboxId.toString());
        }

        void
        setMailboxSelected(std::vector<javelin::protocol::MailboxSelectionSettings>& selections,
                           const QStringView accountId, const QStringView mailboxId,
                           const bool selected)
        {
            auto account =
                std::ranges::find(selections, accountId.toString(),
                                  &javelin::protocol::MailboxSelectionSettings::accountId);
            if (account == selections.end())
            {
                if (!selected)
                    return;
                selections.push_back(
                    {.accountId = accountId.toString(), .mailboxIds = {mailboxId.toString()}});
                return;
            }
            if (selected)
            {
                if (!std::ranges::contains(account->mailboxIds, mailboxId.toString()))
                    account->mailboxIds.push_back(mailboxId.toString());
            }
            else
            {
                std::erase(account->mailboxIds, mailboxId.toString());
            }
        }

        [[nodiscard]] QCoro::Task<std::optional<MailboxVisibilityChangeFailure>>
        applyMailboxVisibilityChanges(javelin::app::MailCommandPort& mailCommands,
                                      std::vector<MailboxVisibilityChangeRequest> changes)
        {
            for (const auto& change : changes)
            {
                auto result = co_await mailCommands.setMailboxSubscribed(
                    change.accountId, change.mailboxId, change.subscribed);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                    co_return MailboxVisibilityChangeFailure{.change = change, .error = *error};
            }
            co_return std::nullopt;
        }

        [[nodiscard]] QString formattedBytes(const std::uint64_t bytes)
        {
            return QLocale{}.formattedDataSize(static_cast<qint64>(bytes));
        }

        [[nodiscard]] QCoro::Task<std::optional<javelin::jmap::cache::DatabaseError>>
        enqueueMailboxCacheClears(javelin::app::DeveloperMaintenancePort& maintenance,
                                  std::vector<MailboxCacheClearRequest> requests)
        {
            std::optional<javelin::jmap::cache::DatabaseError> firstError;
            for (const auto& request : requests)
            {
                if (!request.clearBodies && !request.clearSqlite)
                    continue;

                const auto kind = request.clearSqlite
                                      ? javelin::app::DeveloperMailboxCacheKind::SqliteAndBodies
                                      : javelin::app::DeveloperMailboxCacheKind::Bodies;
                auto result = co_await maintenance.clearMailboxCache(
                    {.accountId = request.accountId,
                     .mailboxId = request.mailboxId,
                     .kind = kind,
                     .offlinePolicy = javelin::app::DeveloperOfflineClearPolicy::Preserve});
                if (!firstError.has_value())
                {
                    if (const auto* error =
                            std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                    {
                        firstError = *error;
                    }
                }
            }
            co_return firstError;
        }

        [[nodiscard]] ConnectionSettings newAccount()
        {
            return ConnectionSettings{
                .id = QUuid::createUuid().toString(QUuid::WithoutBraces),
                .revision = 1,
                .displayName = {},
                .sessionUrl = {},
                .loginEmail = {},
                .tokenEndpoint = {},
                .oauthClientId = {},
                .hasCredentials = false,
                .credentialHandle = {},
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
        javelin::app::MailCommandPort& mailCommandPort,
        javelin::app::OnboardingPort& onboardingPort,
        javelin::gui::translation::TranslationService& translationService,
        javelin::jmap::cache::AccountReader& accountReader,
        javelin::jmap::cache::MailboxReader& mailboxReader,
        javelin::app::DeveloperDiagnosticsPort& developerDiagnosticsPort,
        javelin::app::DeveloperMaintenancePort& developerMaintenancePort, QWidget* parent)
        : KConfigDialog(parent, QStringLiteral("preferences"), nullptr), m_settings(settings),
          m_baseRevision(m_settings.snapshot().revision), m_accountCommandPort(accountCommandPort),
          m_mailCommandPort(mailCommandPort), m_onboardingPort(onboardingPort),
          m_translationService(translationService), m_accountReader(accountReader),
          m_mailboxReader(mailboxReader), m_developerDiagnosticsPort(developerDiagnosticsPort),
          m_developerMaintenancePort(developerMaintenancePort), m_accounts(m_settings.accounts()),
          m_remoteContentSenders(m_settings.remoteContentSenders()),
          m_remoteContentDomains(m_settings.remoteContentDomains()),
          m_translationSettings(m_translationService.settings()),
          m_autoTranslateSenders(m_translationSettings.autoTranslateSenders),
          m_autoTranslateDomains(m_translationSettings.autoTranslateDomains),
          m_messageAppearanceSettings(m_settings.messageAppearanceSettings()),
          m_attachmentSaveSettings(m_settings.attachmentSaveSettings()),
          m_undoSendDelaySeconds(m_settings.undoSendDelaySeconds()),
          m_undoSendUsesDialog(m_settings.undoSendUsesDialog())
    {
        setWindowTitle(i18n("Preferences"));
        resize(760, 420);

        auto* accountsPage = new AccountsPage(this);
        m_accountList = accountsPage->accountList();
        auto* addButton = accountsPage->addButton();
        m_removeButton = accountsPage->removeButton();
        m_reauthenticateButton = accountsPage->reauthenticateButton();
        m_displayNameEdit = accountsPage->displayNameEdit();
        m_loginEmailLabel = accountsPage->loginEmailLabel();
        m_sessionUrlLabel = accountsPage->sessionUrlLabel();
        addPage(accountsPage, i18n("Accounts"), QStringLiteral("user-identity"), QString{}, false);

        auto* mailboxSyncPage =
            new MailboxSyncPage(m_settings, m_accountReader, m_mailboxReader, this);
        m_mailboxSyncAccount = mailboxSyncPage->accountCombo();
        m_mailboxSyncList = mailboxSyncPage->treeView();
        m_mailboxSyncModel = mailboxSyncPage->model();
        addPage(mailboxSyncPage, i18n("Mailboxes"), QStringLiteral("view-refresh"), QString{},
                false);

        auto* remoteContentPage = new RemoteContentPage(this);
        m_remoteContentList = remoteContentPage->permitList();
        m_removeRemoteContentButton = remoteContentPage->removeButton();
        addPage(remoteContentPage, i18n("Remote Content"), QStringLiteral("network-wireless-on"),
                QString{}, false);

        auto* appearancePage = new AppearancePage(m_messageAppearanceSettings, this);
        m_messageColorMode = appearancePage->messageColorMode();
        addPage(appearancePage, i18n("Appearance"), QStringLiteral("preferences-desktop-theme"),
                QString{}, false);

        auto* translationPage =
            new TranslationPage(m_translationService, m_translationSettings, this);
        m_translationProvider = translationPage->provider();
        m_translationControls = translationPage->translationControls();
        m_translationTargetLanguage = translationPage->targetLanguage();
        m_googleTranslationControls = translationPage->googleControls();
        m_translationApiKeyEdit = translationPage->apiKeyEdit();
        m_localTranslationControls = translationPage->localControls();
        m_localTranslationSource = translationPage->localSource();
        m_localTranslationTarget = translationPage->localTarget();
        m_downloadLocalModelsButton = translationPage->downloadLocalModelsButton();
        m_installedLocalModels = translationPage->installedLocalModels();
        m_removeLocalModelsButton = translationPage->removeLocalModelsButton();
        m_localModelStatus = translationPage->localModelStatus();
        m_autoTranslateList = translationPage->autoTranslateList();
        m_removeAutoTranslateButton = translationPage->removeAutoTranslateButton();
        addPage(translationPage, i18n("Translation"), QStringLiteral("preferences-desktop-locale"),
                QString{}, false);

        auto* attachmentsPage = new AttachmentsPage(m_attachmentSaveSettings, this);
        m_askAttachmentDirectoryRadio = attachmentsPage->askDirectoryRadio();
        m_saveAttachmentDirectoryRadio = attachmentsPage->saveDirectoryRadio();
        m_attachmentDirectoryEdit = attachmentsPage->directoryEdit();
        m_attachmentDirectoryButton = attachmentsPage->directoryButton();
        addPage(attachmentsPage, i18n("Attachments"), QStringLiteral("mail-attachment"), QString{},
                false);

        auto* composingPage = new ComposingPage(m_undoSendDelaySeconds, m_undoSendUsesDialog, this);
        m_undoSendDelaySpinBox = composingPage->undoSendDelaySpinBox();
        m_undoSendPresentationCombo = composingPage->undoSendPresentationCombo();
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
        connect(m_undoSendPresentationCombo, &QComboBox::currentIndexChanged, this,
                [this]
                {
                    m_undoSendUsesDialog = m_undoSendPresentationCombo->currentData().toBool();
                    noteUnsavedChanges();
                });
        connect(m_mailboxSyncAccount, &QComboBox::currentIndexChanged, this,
                [this]
                {
                    storeMailboxPreferences();
                    m_mailboxSyncCurrentAccountId = m_mailboxSyncAccount->currentData().toString();
                    refreshMailboxSyncList();
                });
        connect(m_mailboxSyncModel, &QAbstractItemModel::dataChanged, this,
                [this](const QModelIndex& topLeft, const QModelIndex&, const QList<int>& roles)
                { mailboxPreferencesChanged(topLeft, roles); });

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
        storeMailboxPreferences();

        auto deferredCleanupOffers = std::exchange(m_deferredMailboxCacheCleanupOffers, {});
        for (const auto& offer : deferredCleanupOffers)
            offerMailboxCacheCleanup(offer.accountId, offer.mailboxId, offer.mailboxName);

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

        const auto selections = [this](const auto enabled)
        {
            std::vector<javelin::protocol::MailboxSelectionSettings> result;
            result.reserve(static_cast<std::size_t>(m_mailboxPreferences.size()));
            for (auto account = m_mailboxPreferences.cbegin();
                 account != m_mailboxPreferences.cend(); ++account)
            {
                std::vector<QString> mailboxIds;
                for (const auto& [mailboxId, state] : account.value())
                {
                    if (enabled(state))
                        mailboxIds.push_back(QString::fromStdString(mailboxId));
                }
                std::ranges::sort(mailboxIds);
                result.push_back({.accountId = account.key(), .mailboxIds = std::move(mailboxIds)});
            }
            return result;
        };

        std::vector<MailboxVisibilityChangeRequest> visibilityChanges;
        for (auto account = m_mailboxPreferences.cbegin(); account != m_mailboxPreferences.cend();
             ++account)
        {
            const auto cached = m_mailboxReader.listMailboxTree(account.key().toStdString());
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&cached))
            {
                QMessageBox::critical(this, i18n("Could not read mailbox settings"),
                                      error->message);
                return false;
            }
            const auto& mailboxes =
                std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(cached);
            for (const auto& [mailboxId, state] : account.value())
            {
                const auto mailbox = std::ranges::find(mailboxes, mailboxId,
                                                       &javelin::jmap::cache::MailboxTreeItem::id);
                if (mailbox == mailboxes.end())
                    continue;
                const bool subscribed = !state.hidden;
                if (mailbox->isSubscribed != subscribed)
                {
                    const auto mailboxIdText = QString::fromStdString(mailboxId);
                    const auto& settingsSnapshot = m_settings.snapshot();
                    visibilityChanges.push_back({
                        .accountId = account.key().toStdString(),
                        .mailboxId = mailboxId,
                        .subscribed = subscribed,
                        .previousOffline = mailboxSelected(settingsSnapshot.syncedMailboxSelections,
                                                           account.key(), mailboxIdText),
                        .previousNotifications =
                            mailboxSelected(settingsSnapshot.notificationMailboxSelections,
                                            account.key(), mailboxIdText),
                    });
                }
            }
        }

        javelin::protocol::SettingsUpdate update;
        update.accounts = GuiSettings::protocolAccounts(m_accounts);
        update.syncedMailboxSelections =
            selections([](const auto& state) { return state.offline; });
        update.notificationMailboxSelections =
            selections([](const auto& state) { return state.notifications; });
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
        update.undoSendUsesDialog = m_undoSendUsesDialog;
        if (const auto error = m_settings.update(m_baseRevision, std::move(update)))
        {
            QMessageBox::critical(this, i18n("Could not save preferences"), error->detail);
            return false;
        }
        m_baseRevision = m_settings.snapshot().revision;

        if (!visibilityChanges.empty())
        {
            QPointer<PreferencesDialog> dialog{this};
            QPointer<QWidget> warningParent{parentWidget()};
            auto* settings = &m_settings;
            auto* mailboxReader = &m_mailboxReader;
            auto task =
                applyMailboxVisibilityChanges(m_mailCommandPort, std::move(visibilityChanges));
            QCoro::connect(
                std::move(task), QCoreApplication::instance(),
                [dialog, warningParent, settings,
                 mailboxReader](std::optional<MailboxVisibilityChangeFailure> failure)
                {
                    if (!failure.has_value())
                        return;

                    QString rollbackFailure;
                    if (dialog != nullptr || warningParent != nullptr)
                    {
                        const auto accountId = QString::fromStdString(failure->change.accountId);
                        const auto mailboxId = QString::fromStdString(failure->change.mailboxId);
                        const auto cached =
                            mailboxReader->listMailboxTree(failure->change.accountId);
                        if (const auto* mailboxes =
                                std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(
                                    &cached))
                        {
                            const auto mailbox =
                                std::ranges::find(*mailboxes, failure->change.mailboxId,
                                                  &javelin::jmap::cache::MailboxTreeItem::id);
                            if (mailbox != mailboxes->end() &&
                                mailbox->isSubscribed != failure->change.subscribed)
                            {
                                const auto& snapshot = settings->snapshot();
                                auto synced = snapshot.syncedMailboxSelections;
                                auto notifications = snapshot.notificationMailboxSelections;
                                setMailboxSelected(synced, accountId, mailboxId,
                                                   failure->change.previousOffline);
                                setMailboxSelected(notifications, accountId, mailboxId,
                                                   failure->change.previousNotifications);
                                javelin::protocol::SettingsUpdate rollback;
                                rollback.syncedMailboxSelections = std::move(synced);
                                rollback.notificationMailboxSelections = std::move(notifications);
                                if (const auto error =
                                        settings->update(snapshot.revision, std::move(rollback)))
                                {
                                    rollbackFailure = error->detail;
                                }
                            }
                        }
                    }

                    if (dialog != nullptr)
                        dialog->noteUnsavedChanges();
                    if (warningParent != nullptr)
                    {
                        auto message = failure->error.message;
                        if (!rollbackFailure.isEmpty())
                        {
                            message += i18n("\n\nThe previous background settings could not be "
                                            "restored: %1",
                                            rollbackFailure);
                        }
                        QMessageBox::warning(warningParent,
                                             i18n("Could not change mailbox visibility"), message);
                    }
                });
        }

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

        std::vector<MailboxCacheClearRequest> cacheClearRequests;
        cacheClearRequests.reserve(m_pendingMailboxCacheClears.size());
        for (const auto& pending : m_pendingMailboxCacheClears)
        {
            const auto account = m_mailboxPreferences.find(pending.accountId);
            if (account != m_mailboxPreferences.end())
            {
                const auto mailbox = account.value().find(pending.mailboxId.toStdString());
                if (mailbox != account.value().end() && mailbox->second.offline)
                    continue;
            }
            cacheClearRequests.push_back(
                {.accountId = pending.accountId,
                 .mailboxId = pending.mailboxId,
                 .clearSqlite = pending.clearSqlite,
                 .clearBodies = pending.clearBodies || pending.clearSqlite});
        }
        m_pendingMailboxCacheClears.clear();
        if (!cacheClearRequests.empty())
        {
            QPointer<QWidget> cleanupParent{parentWidget()};
            auto task = enqueueMailboxCacheClears(m_developerMaintenancePort,
                                                  std::move(cacheClearRequests));
            QCoro::connect(std::move(task), QCoreApplication::instance(),
                           [cleanupParent](std::optional<javelin::jmap::cache::DatabaseError> error)
                           {
                               if (error.has_value() && cleanupParent != nullptr)
                               {
                                   QMessageBox::warning(
                                       cleanupParent, i18n("Could not queue mailbox cache cleanup"),
                                       error->message);
                               }
                           });
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
        m_mailboxPreferences.clear();
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
            if (!account.hasMailCapability)
                continue;

            const auto accountId = QString::fromStdString(account.accountId);
            const auto configuredAccount = m_settings.accountForCachedId(accountId);
            const auto accountName =
                !configuredAccount.displayName.isEmpty()
                    ? configuredAccount.displayName
                    : (account.name.empty() ? i18n("Unnamed account")
                                            : QString::fromStdString(account.name));
            m_mailboxSyncAccount->addItem(accountName, accountId);

            const auto synced = m_settings.syncedMailboxIds(accountId);
            const auto notifications = m_settings.notificationMailboxIds(accountId);
            std::unordered_map<std::string, javelin::gui::mailboxes::MailboxPreferenceState>
                preferences;
            const auto mailboxResult = m_mailboxReader.listMailboxTree(account.accountId);
            if (const auto* mailboxes =
                    std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&mailboxResult))
            {
                preferences.reserve(mailboxes->size());
                for (const auto& mailbox : *mailboxes)
                {
                    auto state = javelin::gui::mailboxes::MailboxPreferenceState{
                        .offline = synced.contains(QString::fromStdString(mailbox.id)),
                        .notifications = notifications.contains(QString::fromStdString(mailbox.id)),
                        .hidden = !mailbox.isSubscribed,
                    };
                    if (state.hidden)
                    {
                        state = javelin::gui::mailboxes::withMailboxPreference(
                            state, javelin::gui::mailboxes::MailboxPreference::Hidden, true);
                    }
                    preferences.emplace(mailbox.id, state);
                }
            }
            m_mailboxPreferences.insert(accountId, std::move(preferences));
        }
        m_mailboxSyncCurrentAccountId = m_mailboxSyncAccount->currentData().toString();
        refreshMailboxSyncList();
    }

    void PreferencesDialog::refreshMailboxSyncList()
    {
        QSignalBlocker blocker{m_mailboxSyncModel};
        const auto accountId = m_mailboxSyncCurrentAccountId;
        if (accountId.isEmpty())
        {
            m_mailboxSyncModel->setMailboxPreferences({});
            m_mailboxSyncModel->setAccountId(std::string{});
            return;
        }
        m_mailboxSyncModel->setAccountId(std::optional<std::string>{accountId.toStdString()});
        m_mailboxSyncModel->setMailboxPreferences(m_mailboxPreferences.value(accountId));
        m_mailboxSyncList->expandAll();
    }

    void PreferencesDialog::storeMailboxPreferences()
    {
        const auto accountId = m_mailboxSyncCurrentAccountId;
        if (accountId.isEmpty())
            return;
        m_mailboxPreferences.insert(accountId, m_mailboxSyncModel->mailboxPreferences());
    }

    void PreferencesDialog::mailboxPreferencesChanged(const QModelIndex& index,
                                                      const QList<int>& roles)
    {
        if (!roles.isEmpty() && !roles.contains(Qt::CheckStateRole))
            return;

        const QString accountId =
            index.data(javelin::gui::mailboxes::MailboxTreeModel::AccountIdRole).toString();
        const QString mailboxId =
            index.data(javelin::gui::mailboxes::MailboxTreeModel::MailboxIdRole).toString();
        const QString mailboxName =
            index.data(javelin::gui::mailboxes::MailboxTreeModel::MailboxNameRole).toString();
        if (accountId.isEmpty() || mailboxId.isEmpty())
            return;

        javelin::gui::mailboxes::MailboxPreferenceState previous;
        const auto previousAccount = m_mailboxPreferences.find(accountId);
        if (previousAccount != m_mailboxPreferences.end())
        {
            const auto previousMailbox = previousAccount.value().find(mailboxId.toStdString());
            if (previousMailbox != previousAccount.value().end())
                previous = previousMailbox->second;
        }

        storeMailboxPreferences();
        const auto currentAccount = m_mailboxPreferences.find(accountId);
        if (currentAccount == m_mailboxPreferences.end())
            return;
        const auto currentMailbox = currentAccount.value().find(mailboxId.toStdString());
        if (currentMailbox == currentAccount.value().end())
            return;
        const auto current = currentMailbox->second;
        noteUnsavedChanges();

        if (current.offline)
        {
            std::erase_if(
                m_pendingMailboxCacheClears, [&](const PendingMailboxCacheClear& pending)
                { return pending.accountId == accountId && pending.mailboxId == mailboxId; });
            std::erase_if(m_deferredMailboxCacheCleanupOffers,
                          [&](const DeferredMailboxCacheCleanupOffer& offer)
                          { return offer.accountId == accountId && offer.mailboxId == mailboxId; });
            return;
        }
        if (!previous.offline || !m_settings.syncedMailboxIds(accountId).contains(mailboxId))
            return;

        if (current.hidden && !previous.hidden)
        {
            const auto existing = std::ranges::find_if(
                m_deferredMailboxCacheCleanupOffers,
                [&](const DeferredMailboxCacheCleanupOffer& offer)
                { return offer.accountId == accountId && offer.mailboxId == mailboxId; });
            if (existing == m_deferredMailboxCacheCleanupOffers.end())
            {
                m_deferredMailboxCacheCleanupOffers.push_back(
                    {.accountId = accountId, .mailboxId = mailboxId, .mailboxName = mailboxName});
            }
            return;
        }

        QTimer::singleShot(0, this, [this, accountId, mailboxId, mailboxName]
                           { offerMailboxCacheCleanup(accountId, mailboxId, mailboxName); });
    }

    void PreferencesDialog::offerMailboxCacheCleanup(const QString& accountId,
                                                     const QString& mailboxId,
                                                     const QString& mailboxName)
    {
        const auto account = m_mailboxPreferences.find(accountId);
        if (account == m_mailboxPreferences.end())
            return;
        const auto preference = account.value().find(mailboxId.toStdString());
        if (preference == account.value().end() || preference->second.offline ||
            !m_settings.syncedMailboxIds(accountId).contains(mailboxId))
        {
            return;
        }

        std::erase_if(m_pendingMailboxCacheClears, [&](const PendingMailboxCacheClear& pending)
                      { return pending.accountId == accountId && pending.mailboxId == mailboxId; });

        QDialog dialog{this};
        dialog.setWindowTitle(i18n("Clear Offline Mail Cache"));
        dialog.setModal(true);
        dialog.setMinimumWidth(520);

        auto* layout = new QVBoxLayout(&dialog);
        auto* description = new QLabel(
            i18n("Offline sync has been disabled for %1. You can keep its downloaded cache or "
                 "remove some of it now.",
                 mailboxName),
            &dialog);
        description->setWordWrap(true);
        layout->addWidget(description);

        auto* calculationStatus = new QLabel(i18n("Calculating reclaimable space…"), &dialog);
        calculationStatus->setWordWrap(true);
        layout->addWidget(calculationStatus);
        auto* progress = new QProgressBar(&dialog);
        progress->setRange(0, 0);
        progress->setTextVisible(false);
        layout->addWidget(progress);

        auto* clearSqlite =
            new QCheckBox(i18n("Clear cached mail list and metadata (calculating…)"), &dialog);
        auto* clearBodies = new QCheckBox(
            i18n("Clear cached message bodies and attachments (calculating…)"), &dialog);
        clearSqlite->setEnabled(false);
        clearBodies->setEnabled(false);
        layout->addWidget(clearSqlite);
        layout->addWidget(clearBodies);

        auto* dependency = new QLabel(
            i18n("Cached mail database entries may only be cleared together with their message "
                 "bodies."),
            &dialog);
        dependency->setWordWrap(true);
        dependency->setForegroundRole(QPalette::PlaceholderText);
        layout->addWidget(dependency);

        auto* selectionSummary = new QLabel(&dialog);
        selectionSummary->setWordWrap(true);
        layout->addWidget(selectionSummary);

        auto* buttons =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        auto* continueButton = buttons->button(QDialogButtonBox::Ok);
        continueButton->setText(i18nc("@action:button", "Continue"));
        continueButton->setEnabled(false);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);

        std::uint64_t reclaimableBodyBytes = 0;
        bool measurementReady = false;
        const auto updateSelectionSummary = [&]
        {
            if (!measurementReady)
                return;
            if (clearBodies->isChecked())
            {
                selectionSummary->setText(
                    i18n("Up to %1 of message-body disk space can be reclaimed.",
                         formattedBytes(reclaimableBodyBytes)));
            }
            else
            {
                selectionSummary->setText(i18n("Downloaded message bodies will be kept."));
            }
        };
        connect(clearSqlite, &QCheckBox::toggled, &dialog,
                [&, clearBodies](const bool checked)
                {
                    if (checked)
                        clearBodies->setChecked(true);
                    clearBodies->setEnabled(measurementReady && !checked);
                    updateSelectionSummary();
                });
        connect(clearBodies, &QCheckBox::toggled, &dialog,
                [&updateSelectionSummary] { updateSelectionSummary(); });

        auto task = m_developerDiagnosticsPort.snapshot();
        QCoro::connect(
            std::move(task), &dialog,
            [&](javelin::app::DeveloperDiagnosticsResult result)
            {
                progress->hide();
                if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                {
                    calculationStatus->setText(
                        i18n("Could not calculate reclaimable space: %1", error->message));
                    buttons->button(QDialogButtonBox::Cancel)
                        ->setText(i18nc("@action:button", "Close"));
                    return;
                }

                const auto& snapshot = std::get<javelin::app::DeveloperDiagnosticsSnapshot>(result);
                const auto mailbox = std::ranges::find_if(
                    snapshot.mailboxes, [&](const javelin::app::DeveloperMailboxRecord& record)
                    { return record.accountId == accountId && record.mailboxId == mailboxId; });
                if (mailbox == snapshot.mailboxes.end())
                {
                    calculationStatus->setText(
                        i18n("The mailbox is no longer available in the local cache."));
                    buttons->button(QDialogButtonBox::Cancel)
                        ->setText(i18nc("@action:button", "Close"));
                    return;
                }

                reclaimableBodyBytes = mailbox->usage.reclaimableBodyBytes;
                clearSqlite->setText(i18n("Clear cached mail list and metadata (about %1)",
                                          formattedBytes(mailbox->usage.sqliteEstimatedBytes)));
                clearBodies->setText(
                    i18n("Clear cached message bodies and attachments (up to %1 reclaimable)",
                         formattedBytes(reclaimableBodyBytes)));
                calculationStatus->setText(i18n("Reclaimable space calculated."));
                measurementReady = true;
                clearSqlite->setEnabled(true);
                clearBodies->setEnabled(true);
                continueButton->setEnabled(true);
                updateSelectionSummary();
            });

        if (dialog.exec() != QDialog::Accepted)
            return;

        const bool clearSqliteSelected = clearSqlite->isChecked();
        const bool clearBodiesSelected = clearBodies->isChecked() || clearSqliteSelected;
        if (!clearSqliteSelected && !clearBodiesSelected)
            return;

        m_pendingMailboxCacheClears.push_back({.accountId = accountId,
                                               .mailboxId = mailboxId,
                                               .clearSqlite = clearSqliteSelected,
                                               .clearBodies = clearBodiesSelected});
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
