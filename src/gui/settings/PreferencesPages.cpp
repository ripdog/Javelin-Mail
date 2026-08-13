#include "gui/settings/PreferencesPages.h"

#include "gui/mailboxes/MailboxTreeModel.h"
#include "gui/mailboxes/MailboxTreeView.h"
#include "gui/translation/TranslationService.h"
#include "jmap/cache/AccountReadRepository.h"
#include "jmap/cache/MailboxReadRepository.h"

#include <KLocalizedString>

#include <QAbstractItemView>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QSizePolicy>
#include <QSpinBox>
#include <QSplitter>
#include <QVBoxLayout>

namespace javelin::gui::settings
{
    AccountsPage::AccountsPage(QWidget* parent) : QWidget(parent)
    {
        auto* pageLayout = new QVBoxLayout(this);
        auto* splitter = new QSplitter(this);
        auto* accountPanel = new QWidget(splitter);
        auto* accountLayout = new QVBoxLayout(accountPanel);
        accountLayout->addWidget(new QLabel(i18n("Accounts"), accountPanel));
        m_accountList = new QListWidget(accountPanel);
        accountLayout->addWidget(m_accountList, 1);

        auto* accountButtons = new QHBoxLayout();
        m_addButton = new QPushButton(i18nc("@action:button", "Add"), accountPanel);
        m_removeButton = new QPushButton(i18nc("@action:button", "Remove"), accountPanel);
        accountButtons->addWidget(m_addButton);
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
        pageLayout->addWidget(splitter, 1);
    }

    QListWidget* AccountsPage::accountList() const
    {
        return m_accountList;
    }

    QPushButton* AccountsPage::addButton() const
    {
        return m_addButton;
    }

    QPushButton* AccountsPage::removeButton() const
    {
        return m_removeButton;
    }

    QPushButton* AccountsPage::reauthenticateButton() const
    {
        return m_reauthenticateButton;
    }

    QLineEdit* AccountsPage::displayNameEdit() const
    {
        return m_displayNameEdit;
    }

    QLabel* AccountsPage::loginEmailLabel() const
    {
        return m_loginEmailLabel;
    }

    QLabel* AccountsPage::sessionUrlLabel() const
    {
        return m_sessionUrlLabel;
    }

    MailboxSyncPage::MailboxSyncPage(GuiSettings& settings,
                                     javelin::jmap::cache::AccountReader& accountReader,
                                     javelin::jmap::cache::MailboxReader& mailboxReader,
                                     QWidget* parent)
        : QWidget(parent)
    {
        auto* layout = new QVBoxLayout(this);
        auto* description = new QLabel(
            i18n("Choose which mailboxes are kept completely offline, show new-mail "
                 "notifications, or are hidden from the normal mailbox list. Hidden mailboxes "
                 "cannot be kept offline or show notifications; enabling either option shows "
                 "the mailbox again."),
            this);
        description->setWordWrap(true);
        description->setSizePolicy(QSizePolicy{QSizePolicy::Preferred, QSizePolicy::Minimum});
        layout->addWidget(description);
        m_accountCombo = new QComboBox(this);
        layout->addWidget(m_accountCombo);
        m_treeView = new javelin::gui::mailboxes::MailboxTreeView(this);
        m_model = new javelin::gui::mailboxes::MailboxTreeModel(
            accountReader, mailboxReader,
            {.accountId = std::string{},
             .showAccount = false,
             .checkable = false,
             .checkedMailboxIds = {},
             .preferenceColumns = true,
             .includeHidden = true,
             .accountDisplayName = [&settings](const QStringView accountId)
             { return settings.accountForCachedId(accountId).displayName; }},
            m_treeView);
        m_treeView->setModel(m_model);
        m_treeView->setHeaderHidden(false);
        m_treeView->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        for (int column = 1; column < m_model->columnCount(); ++column)
            m_treeView->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
        m_treeView->setSizePolicy(QSizePolicy{QSizePolicy::Ignored, QSizePolicy::Expanding});
        layout->addWidget(m_treeView, 1);
    }

    QComboBox* MailboxSyncPage::accountCombo() const
    {
        return m_accountCombo;
    }

    javelin::gui::mailboxes::MailboxTreeView* MailboxSyncPage::treeView() const
    {
        return m_treeView;
    }

    javelin::gui::mailboxes::MailboxTreeModel* MailboxSyncPage::model() const
    {
        return m_model;
    }

    RemoteContentPage::RemoteContentPage(QWidget* parent) : QWidget(parent)
    {
        auto* layout = new QVBoxLayout(this);
        layout->addWidget(new QLabel(i18n("Allowed Remote Content"), this));
        m_permitList = new QListWidget(this);
        m_permitList->setSelectionMode(QAbstractItemView::ExtendedSelection);
        layout->addWidget(m_permitList, 1);
        auto* buttons = new QHBoxLayout();
        buttons->addStretch(1);
        m_removeButton = new QPushButton(i18nc("@action:button", "Remove"), this);
        buttons->addWidget(m_removeButton);
        layout->addLayout(buttons);
    }

    QListWidget* RemoteContentPage::permitList() const
    {
        return m_permitList;
    }

    QPushButton* RemoteContentPage::removeButton() const
    {
        return m_removeButton;
    }

    AppearancePage::AppearancePage(
        const javelin::gui::messageview::MessageAppearanceSettings& settings, QWidget* parent)
        : QWidget(parent)
    {
        auto* layout = new QFormLayout(this);
        m_messageColorMode = new QComboBox(this);
        m_messageColorMode->addItem(
            i18n("Follow application"),
            static_cast<int>(javelin::gui::messageview::MessageColorMode::FollowApplication));
        m_messageColorMode->addItem(
            i18n("Always use original colours"),
            static_cast<int>(javelin::gui::messageview::MessageColorMode::Light));
        m_messageColorMode->addItem(
            i18n("Always use dark colours"),
            static_cast<int>(javelin::gui::messageview::MessageColorMode::Dark));
        const int index = m_messageColorMode->findData(static_cast<int>(settings.colorMode));
        m_messageColorMode->setCurrentIndex(index);
        layout->addRow(i18n("HTML message colours"), m_messageColorMode);
    }

    QComboBox* AppearancePage::messageColorMode() const
    {
        return m_messageColorMode;
    }

    TranslationPage::TranslationPage(javelin::gui::translation::TranslationService& service,
                                     const javelin::gui::translation::TranslationSettings& settings,
                                     QWidget* parent)
        : QWidget(parent)
    {
        auto* layout = new QVBoxLayout(this);
        auto* providerForm = new QFormLayout();
        m_provider = new QComboBox(this);
        m_provider->addItem(
            i18nc("@item translation provider", "Disabled"),
            static_cast<int>(javelin::gui::translation::TranslationProvider::Disabled));
        m_provider->addItem(
            i18nc("@item translation provider", "Google Translate"),
            static_cast<int>(javelin::gui::translation::TranslationProvider::Google));
        if (service.localProviderAvailable())
        {
            m_provider->addItem(
                i18nc("@item translation provider", "Local (Firefox models)"),
                static_cast<int>(javelin::gui::translation::TranslationProvider::Local));
        }
        const auto providerIndex = m_provider->findData(static_cast<int>(settings.provider));
        m_provider->setCurrentIndex(providerIndex >= 0 ? providerIndex : 1);
        providerForm->addRow(i18n("Translation provider"), m_provider);
        layout->addLayout(providerForm);

        m_translationControls = new QWidget(this);
        auto* controlsLayout = new QVBoxLayout(m_translationControls);
        controlsLayout->setContentsMargins(0, 0, 0, 0);
        auto* translationForm = new QFormLayout();
        m_targetLanguage = new QComboBox(m_translationControls);
        m_targetLanguage->setEditable(true);
        m_targetLanguage->addItem(i18n("English"), QStringLiteral("en"));
        m_targetLanguage->addItem(i18n("Chinese (Simplified)"), QStringLiteral("zh-Hans"));
        m_targetLanguage->addItem(i18n("Chinese (Traditional)"), QStringLiteral("zh-Hant"));
        m_targetLanguage->addItem(i18n("French"), QStringLiteral("fr"));
        m_targetLanguage->addItem(i18n("German"), QStringLiteral("de"));
        m_targetLanguage->addItem(i18n("Italian"), QStringLiteral("it"));
        m_targetLanguage->addItem(i18n("Japanese"), QStringLiteral("ja"));
        m_targetLanguage->addItem(i18n("Korean"), QStringLiteral("ko"));
        m_targetLanguage->addItem(i18n("Portuguese"), QStringLiteral("pt"));
        m_targetLanguage->addItem(i18n("Russian"), QStringLiteral("ru"));
        m_targetLanguage->addItem(i18n("Spanish"), QStringLiteral("es"));
        const int targetIndex = m_targetLanguage->findData(settings.targetLanguage);
        if (targetIndex >= 0)
            m_targetLanguage->setCurrentIndex(targetIndex);
        else
            m_targetLanguage->setEditText(settings.targetLanguage);
        translationForm->addRow(i18n("Target language"), m_targetLanguage);
        controlsLayout->addLayout(translationForm);

        m_googleControls = new QWidget(m_translationControls);
        auto* googleLayout = new QVBoxLayout(m_googleControls);
        googleLayout->setContentsMargins(0, 0, 0, 0);
        auto* googleDescription = new QLabel(
            i18n("Translated message text is sent to Google Translate. Leave the API key empty "
                 "to use Javelin's built-in default key."),
            m_googleControls);
        googleDescription->setWordWrap(true);
        googleLayout->addWidget(googleDescription);
        auto* googleForm = new QFormLayout();
        m_apiKeyEdit = new QLineEdit(m_googleControls);
        m_apiKeyEdit->setEchoMode(QLineEdit::Password);
        m_apiKeyEdit->setPlaceholderText(i18n("Use built-in default key"));
        m_apiKeyEdit->setText(settings.apiKeyOverride);
        googleForm->addRow(i18n("API key override"), m_apiKeyEdit);
        googleLayout->addLayout(googleForm);
        controlsLayout->addWidget(m_googleControls);

        m_localControls = new QWidget(m_translationControls);
        auto* localLayout = new QVBoxLayout(m_localControls);
        localLayout->setContentsMargins(0, 0, 0, 0);
        auto* localDescription = new QLabel(
            i18n("Local translation runs only in the Javelin GUI. Firefox-compatible models are "
                 "downloaded on demand and remain on this computer."),
            m_localControls);
        localDescription->setWordWrap(true);
        localLayout->addWidget(localDescription);
        auto* localRouteLayout = new QHBoxLayout();
        m_localSource = new QComboBox(m_localControls);
        m_localTarget = new QComboBox(m_localControls);
        for (const auto& language : service.localSourceLanguages())
            m_localSource->addItem(language, language);
        auto* sourceLabel =
            new QLabel(i18nc("@label translation source language", "From"), m_localControls);
        sourceLabel->setBuddy(m_localSource);
        localRouteLayout->addWidget(sourceLabel);
        localRouteLayout->addWidget(m_localSource, 1);
        auto* targetLabel =
            new QLabel(i18nc("@label translation target language", "to"), m_localControls);
        targetLabel->setBuddy(m_localTarget);
        localRouteLayout->addWidget(targetLabel);
        localRouteLayout->addWidget(m_localTarget, 1);
        m_downloadLocalModelsButton = new QPushButton(i18n("Download models"), m_localControls);
        localRouteLayout->addWidget(m_downloadLocalModelsButton);
        localLayout->addLayout(localRouteLayout);
        m_localModelStatus = new QLabel(m_localControls);
        m_localModelStatus->setWordWrap(true);
        localLayout->addWidget(m_localModelStatus);
        auto* installedLabel = new QLabel(i18n("Downloaded model directions"), m_localControls);
        m_installedLocalModels = new QListWidget(m_localControls);
        installedLabel->setBuddy(m_installedLocalModels);
        m_installedLocalModels->setSelectionMode(QAbstractItemView::ExtendedSelection);
        localLayout->addWidget(installedLabel);
        localLayout->addWidget(m_installedLocalModels);
        auto* localButtons = new QHBoxLayout();
        localButtons->addStretch(1);
        m_removeLocalModelsButton =
            new QPushButton(i18nc("@action:button", "Remove"), m_localControls);
        localButtons->addWidget(m_removeLocalModelsButton);
        localLayout->addLayout(localButtons);
        controlsLayout->addWidget(m_localControls);

        auto* autoTranslateLabel =
            new QLabel(i18n("Auto-Translate Entries"), m_translationControls);
        m_autoTranslateList = new QListWidget(m_translationControls);
        autoTranslateLabel->setBuddy(m_autoTranslateList);
        m_autoTranslateList->setSelectionMode(QAbstractItemView::ExtendedSelection);
        controlsLayout->addWidget(autoTranslateLabel);
        controlsLayout->addWidget(m_autoTranslateList, 1);
        auto* buttons = new QHBoxLayout();
        buttons->addStretch(1);
        m_removeAutoTranslateButton =
            new QPushButton(i18nc("@action:button", "Remove"), m_translationControls);
        buttons->addWidget(m_removeAutoTranslateButton);
        controlsLayout->addLayout(buttons);
        layout->addWidget(m_translationControls, 1);
    }

    QComboBox* TranslationPage::provider() const
    {
        return m_provider;
    }
    QWidget* TranslationPage::translationControls() const
    {
        return m_translationControls;
    }
    QComboBox* TranslationPage::targetLanguage() const
    {
        return m_targetLanguage;
    }
    QWidget* TranslationPage::googleControls() const
    {
        return m_googleControls;
    }
    QLineEdit* TranslationPage::apiKeyEdit() const
    {
        return m_apiKeyEdit;
    }
    QWidget* TranslationPage::localControls() const
    {
        return m_localControls;
    }
    QComboBox* TranslationPage::localSource() const
    {
        return m_localSource;
    }
    QComboBox* TranslationPage::localTarget() const
    {
        return m_localTarget;
    }
    QPushButton* TranslationPage::downloadLocalModelsButton() const
    {
        return m_downloadLocalModelsButton;
    }
    QListWidget* TranslationPage::installedLocalModels() const
    {
        return m_installedLocalModels;
    }
    QPushButton* TranslationPage::removeLocalModelsButton() const
    {
        return m_removeLocalModelsButton;
    }
    QLabel* TranslationPage::localModelStatus() const
    {
        return m_localModelStatus;
    }
    QListWidget* TranslationPage::autoTranslateList() const
    {
        return m_autoTranslateList;
    }
    QPushButton* TranslationPage::removeAutoTranslateButton() const
    {
        return m_removeAutoTranslateButton;
    }

    AttachmentsPage::AttachmentsPage(const AttachmentSaveSettings& settings, QWidget* parent)
        : QWidget(parent)
    {
        auto* layout = new QVBoxLayout(this);
        m_askDirectoryRadio = new QRadioButton(i18n("Always ask where to save attachments"), this);
        m_saveDirectoryRadio = new QRadioButton(i18n("Always save attachments to:"), this);
        layout->addWidget(m_askDirectoryRadio);
        layout->addWidget(m_saveDirectoryRadio);
        auto* directoryLayout = new QHBoxLayout();
        m_directoryEdit = new QLineEdit(this);
        m_directoryEdit->setReadOnly(true);
        m_directoryButton = new QPushButton(i18n("Choose..."), this);
        directoryLayout->addWidget(m_directoryEdit, 1);
        directoryLayout->addWidget(m_directoryButton);
        layout->addLayout(directoryLayout);
        layout->addStretch(1);
        m_askDirectoryRadio->setChecked(settings.alwaysAsk);
        m_saveDirectoryRadio->setChecked(!settings.alwaysAsk);
        m_directoryEdit->setText(settings.directory);
    }

    QRadioButton* AttachmentsPage::askDirectoryRadio() const
    {
        return m_askDirectoryRadio;
    }
    QRadioButton* AttachmentsPage::saveDirectoryRadio() const
    {
        return m_saveDirectoryRadio;
    }
    QLineEdit* AttachmentsPage::directoryEdit() const
    {
        return m_directoryEdit;
    }
    QPushButton* AttachmentsPage::directoryButton() const
    {
        return m_directoryButton;
    }

    ComposingPage::ComposingPage(const int undoSendDelaySeconds, QWidget* parent) : QWidget(parent)
    {
        auto* layout = new QFormLayout(this);
        m_undoSendDelaySpinBox = new QSpinBox(this);
        m_undoSendDelaySpinBox->setRange(1, 120);
        m_undoSendDelaySpinBox->setSuffix(i18nc("@item time suffix", " seconds"));
        m_undoSendDelaySpinBox->setValue(undoSendDelaySeconds);
        layout->addRow(i18n("Undo send window:"), m_undoSendDelaySpinBox);
    }

    QSpinBox* ComposingPage::undoSendDelaySpinBox() const
    {
        return m_undoSendDelaySpinBox;
    }
} // namespace javelin::gui::settings
