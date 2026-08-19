#include "gui/compose/ComposeTabWidget.h"

#include "app/ComposeApplicationPorts.h"
#include "gui/compose/AttachmentController.h"
#include "gui/compose/ComposeAutosaveController.h"

#include "gui/compose/ComposeBodyConverter.h"
#include "gui/compose/ComposeIdentityController.h"
#include "gui/compose/ComposeRecipientController.h"
#include "gui/compose/ComposeUiPreferences.h"
#include "gui/compose/ComposerInlineImageCodec.h"
#include "gui/compose/InlineImageController.h"
#include "gui/compose/JavelinComposerEdit.h"
#include "gui/compose/SignatureController.h"
#include "gui/messageview/HtmlMessageView.h"
#include "gui/settings/ConnectionSettingsAdapter.h"
#include "gui/settings/GuiSettings.h"
#include "jmap/cache/AccountReadRepository.h"
#include "jmap/cache/IdentityReader.h"
#include "jmap/contacts/ContactIdentityLookup.h"

#include <QCoroTask>

#include <KActionCollection>
#include <KLocalizedString>
#include <KPIMTextEdit/RichTextComposerControler>
#include <KPIMTextEdit/RichTextComposerImages>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStringList>
#include <QStyle>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTimeZone>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QVariant>
#include <QtConcurrentRun>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>

namespace javelin::gui::compose
{
    Q_LOGGING_CATEGORY(logComposeImage, "gui.compose.image")

    namespace
    {

        constexpr auto richEditorTabIndex = 0;
        constexpr auto previewTabIndex = 1;
        constexpr auto senderIdentityIdRole = ComposeIdentityController::identityIdRole;
        constexpr auto senderAccountIdRole = ComposeIdentityController::accountIdRole;
        constexpr auto senderEmailRole = ComposeIdentityController::emailRole;
        constexpr auto senderTextSignatureRole = ComposeIdentityController::textSignatureRole;
        constexpr auto senderHtmlSignatureRole = ComposeIdentityController::htmlSignatureRole;
        constexpr auto senderBccRole = ComposeIdentityController::bccRole;

        struct ImageInsertTiming
        {
            QElapsedTimer elapsed;
            std::optional<qint64> acceptedAtMilliseconds;
            QString sourceFilePath;
            QMetaObject::Connection documentChangeConnection;
        };

        [[nodiscard]] QString imageDialogSourceFilePath(const QDialog& dialog)
        {
            for (const auto* lineEdit : dialog.findChildren<QLineEdit*>())
            {
                const auto value = lineEdit->text().trimmed();
                if (value.isEmpty())
                    continue;

                const auto url =
                    QUrl::fromUserInput(value, QDir::currentPath(), QUrl::AssumeLocalFile);
                const auto path = url.isLocalFile() ? url.toLocalFile() : value;
                const QFileInfo file{path};
                if (file.exists() && file.isFile())
                    return file.absoluteFilePath();
            }
            return {};
        }

        [[nodiscard]] QString defaultTitleForMode(const javelin::jmap::submission::ComposeMode mode)
        {
            switch (mode)
            {
            case javelin::jmap::submission::ComposeMode::NewMessage:
                return i18n("New Message");
            case javelin::jmap::submission::ComposeMode::Reply:
                return i18n("Reply");
            case javelin::jmap::submission::ComposeMode::ReplyAll:
                return i18n("Reply All");
            case javelin::jmap::submission::ComposeMode::Forward:
                return i18n("Forward");
            case javelin::jmap::submission::ComposeMode::EditDraft:
                return i18n("Edit Draft");
            }

            return i18n("Compose");
        }

        [[nodiscard]] QString detectedMediaType(const QString& filePath)
        {
            QMimeDatabase mimeDatabase;
            const auto mimeType =
                mimeDatabase.mimeTypeForFile(filePath, QMimeDatabase::MatchContent);
            return mimeType.isValid() ? mimeType.name()
                                      : QStringLiteral("application/octet-stream");
        }

        [[nodiscard]] std::optional<javelin::app::AccountConnectionSettings>
        liveSettings(const javelin::gui::settings::GuiSettings& guiSettings,
                     const std::string_view accountId, QString* errorMessage = nullptr)
        {
            const auto settings =
                guiSettings.accountForCachedId(QString::fromStdString(std::string{accountId}));
            if (settings.sessionUrl.isEmpty() || settings.loginEmail.isEmpty() ||
                !settings.hasCredentials)
            {
                if (errorMessage != nullptr)
                    *errorMessage = i18n("Sign in to this account in Preferences first.");
                return std::nullopt;
            }

            return javelin::gui::settings::toAccountConnectionSettings(settings);
        }

    } // namespace

    ComposeTabWidget::~ComposeTabWidget() = default;

    ComposeTabWidget::ComposeTabWidget(
        javelin::gui::settings::GuiSettings& settings,
        javelin::app::ComposeCommandPort& composeCommandPort,
        javelin::jmap::cache::AccountReader& accountReader,
        javelin::jmap::cache::IdentityReader& identityRepository,
        javelin::jmap::contacts::ContactIdentityLookup& contactIdentityLookup,
        javelin::jmap::submission::DraftSnapshot snapshot, QWidget* parent,
        const bool hasUnsavedChanges)
        : QWidget(parent), m_settings(settings), m_composeCommandPort(composeCommandPort),
          m_accountReader(accountReader), m_contactIdentityLookup(contactIdentityLookup),
          m_snapshot(std::move(snapshot))
    {
        m_autosaveController = new ComposeAutosaveController(
            hasUnsavedChanges, [this] { persistWorkingCopy(); }, this);
        setAcceptDrops(true);
        setupUi();
        m_attachmentController = std::make_unique<AttachmentController>(
            *m_attachmentScrollArea, *m_attachmentStrip, *m_attachmentStripLayout, m_snapshot,
            [this](const std::size_t index) { removeAttachmentAt(index); },
            [this](const std::size_t index, const bool embedded)
            { setAttachmentEmbedded(index, embedded); });
        m_inlineImageController = std::make_unique<InlineImageController>(
            *m_richTextEdit, m_snapshot,
            [this]
            {
                populateAttachments();
                scheduleWorkingCopySave();
            },
            [this](QString message, const int timeoutMs)
            { Q_EMIT statusMessageRequested(message, timeoutMs); },
            [this] { Q_EMIT toolbarStateChanged(); },
            [this](const bool succeeded) { finishInlineImagePreparation(succeeded); });
        createToolbarActions();
        m_identityController = std::make_unique<ComposeIdentityController>(
            m_settings, m_composeCommandPort, m_accountReader, identityRepository, *m_fromCombo,
            *this, [this](QString message, const int timeoutMs)
            { Q_EMIT statusMessageRequested(message, timeoutMs); },
            [this]
            {
                loadIdentities();
                Q_EMIT toolbarStateChanged();
            });
        m_signatureController = std::make_unique<SignatureController>(
            *m_fromCombo, *m_richTextEdit, m_snapshot, [this] { scheduleWorkingCopySave(); });
        loadIdentities();
        applySnapshotToUi();

        connect(
            m_fromCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](const int index)
            {
                if (m_syncingUi)
                {
                    return;
                }

                if (m_previousIdentityIndex >= 0)
                {
                    const auto previousAutomaticBcc =
                        m_fromCombo->itemData(m_previousIdentityIndex, senderBccRole).toString();
                    if (recipientText(RecipientType::Bcc).trimmed() ==
                        previousAutomaticBcc.trimmed())
                    {
                        const auto nextAutomaticBcc =
                            m_fromCombo->itemData(index, senderBccRole).toString();
                        setRecipientText(RecipientType::Bcc, nextAutomaticBcc);
                    }
                }
                replaceTrackedSignatureForIndex(index);
                m_previousIdentityIndex = index;
                syncSnapshotFromUi();
                scheduleWorkingCopySave();
                Q_EMIT toolbarStateChanged();
            });
        connect(m_subjectEdit, &QLineEdit::textChanged, this,
                [this](const QString&)
                {
                    if (m_syncingUi)
                    {
                        return;
                    }
                    updateTabTitle();
                    scheduleWorkingCopySave();
                });

        connect(m_richTextEdit, &JavelinComposerEdit::attachmentPathsRequested, this,
                &ComposeTabWidget::addAttachmentPaths);
        connect(m_richTextEdit, &JavelinComposerEdit::inlineImageRequested, this,
                &ComposeTabWidget::addPastedInlineImage);
        connect(m_richTextEdit, &QTextEdit::textChanged, this,
                [this]
                {
                    if (!m_syncingUi)
                    {
                        scheduleWorkingCopySave();
                    }
                });
        connect(
            m_richTextEdit->document(), &QTextDocument::contentsChange, this,
            [this](const int position, const int removed, const int added)
            { m_signatureController->noteDocumentChange(position, removed, added, m_syncingUi); });
        connect(m_richTextEdit, &QTextEdit::currentCharFormatChanged, this,
                [this](const QTextCharFormat& format)
                {
                    if (m_codeAction != nullptr)
                    {
                        const QSignalBlocker blocker{m_codeAction};
                        m_codeAction->setChecked(format.fontFamilies().toStringList().contains(
                            QStringLiteral("monospace")));
                    }
                });
        connect(m_editorTabs, &QTabWidget::currentChanged, this,
                [this](const int index)
                {
                    if (m_syncingUi)
                    {
                        return;
                    }

                    if (index == previewTabIndex)
                    {
                        syncSnapshotFromUi();
                        refreshPreview();
                    }
                    updateEditorModeUi();
                });

        refreshPreview();
        updateEditorModeUi();
        updateTabTitle();
    }

    QString ComposeTabWidget::tabTitle() const
    {
        const auto subject = m_subjectEdit->text().trimmed();
        return subject.isEmpty() ? defaultTitleForMode(m_snapshot.mode) : subject;
    }

    QString ComposeTabWidget::confirmationDetails() const
    {
        QStringList details;
        const auto subject = m_subjectEdit->text().trimmed();
        details.push_back(i18n("Subject: %1", subject.isEmpty() ? i18n("(no subject)") : subject));

        const auto appendRecipients = [&details](const QString& label, const QString& recipients)
        {
            if (!recipients.trimmed().isEmpty())
                details.push_back(QStringLiteral("%1: %2").arg(label, recipients.trimmed()));
        };
        appendRecipients(i18n("To"), recipientText(RecipientType::To));
        appendRecipients(i18n("Cc"), recipientText(RecipientType::Cc));
        appendRecipients(i18n("Bcc"), recipientText(RecipientType::Bcc));
        if (details.size() == 1)
            details.push_back(i18n("Recipients: (none)"));

        const auto account =
            m_settings.accountForCachedId(QString::fromStdString(m_snapshot.accountId));
        QString accountLabel = account.displayName;
        if (accountLabel.isEmpty())
            accountLabel = account.loginEmail;
        else if (!account.loginEmail.isEmpty() &&
                 accountLabel.compare(account.loginEmail, Qt::CaseInsensitive) != 0)
            accountLabel = QStringLiteral("%1 — %2").arg(accountLabel, account.loginEmail);
        if (!accountLabel.isEmpty())
            details.push_back(i18n("Account: %1", accountLabel));

        return details.join(QLatin1Char('\n'));
    }

    std::string ComposeTabWidget::composeSessionId() const
    {
        return m_snapshot.composeSessionId;
    }

    std::optional<std::string> ComposeTabWidget::draftEmailId() const
    {
        return m_snapshot.draftEmailId;
    }

    bool ComposeTabWidget::isEmptyDraft() const
    {
        const auto subject = m_subjectEdit->text().trimmed();
        const auto body = m_richTextEdit->toPlainText();
        return subject.isEmpty() && recipientText(RecipientType::To).trimmed().isEmpty() &&
               recipientText(RecipientType::Cc).trimmed().isEmpty() &&
               recipientText(RecipientType::Bcc).trimmed().isEmpty() && body.trimmed().isEmpty() &&
               m_snapshot.attachments.empty();
    }

    bool ComposeTabWidget::closeWithoutPrompt() const
    {
        return m_closeWithoutPrompt;
    }

    bool ComposeTabWidget::hasUnsavedChanges() const
    {
        return m_autosaveController->hasUnsavedChanges();
    }

    bool ComposeTabWidget::operationInFlight() const
    {
        return m_operationInFlight;
    }

    bool ComposeTabWidget::canSend() const
    {
        const auto index = m_fromCombo->currentIndex();
        if (m_operationInFlight || index < 0)
            return false;
        return !m_fromCombo->itemData(index, senderAccountIdRole).toString().isEmpty() &&
               !m_fromCombo->itemData(index, senderIdentityIdRole).toString().isEmpty();
    }

    std::optional<std::uint64_t> ComposeTabWidget::currentMaxDelayedSendSeconds() const
    {
        const auto index = m_fromCombo->currentIndex();
        if (index < 0)
            return std::nullopt;
        const auto accountId = m_fromCombo->itemData(index, senderAccountIdRole).toString();
        if (accountId.isEmpty())
            return std::nullopt;
        const auto result = m_accountReader.findById(accountId.toStdString());
        const auto* account =
            std::get_if<std::optional<javelin::jmap::cache::CachedAccount>>(&result);
        if (account == nullptr || !account->has_value() ||
            !account->value().hasSubmissionCapability ||
            account->value().maxDelayedSendSeconds == 0)
            return std::nullopt;
        return account->value().maxDelayedSendSeconds;
    }

    bool ComposeTabWidget::canScheduleSend() const
    {
        return canSend() && currentMaxDelayedSendSeconds().has_value();
    }

    bool ComposeTabWidget::richTextEnabled() const
    {
        return m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::RichText;
    }

    QMenu* ComposeTabWidget::signatureMenu() const
    {
        return m_signatureMenu;
    }

    void ComposeTabWidget::editCurrentSignature()
    {
        if (!canSend())
            return;
        Q_EMIT manageIdentitiesRequested(m_fromCombo->currentData(senderAccountIdRole).toString(),
                                         m_fromCombo->currentData(senderIdentityIdRole).toString());
    }

    void ComposeTabWidget::setRichTextEnabled(const bool enabled)
    {
        switchBodyFormat(enabled);
    }

    void ComposeTabWidget::saveDraftAndClose()
    {
        startSaveDraft(true);
    }

    void ComposeTabWidget::dragEnterEvent(QDragEnterEvent* event)
    {
        if (event->mimeData()->hasUrls())
        {
            for (const auto& url : event->mimeData()->urls())
            {
                if (url.isLocalFile())
                {
                    event->acceptProposedAction();
                    return;
                }
            }
        }

        QWidget::dragEnterEvent(event);
    }

    void ComposeTabWidget::dragMoveEvent(QDragMoveEvent* event)
    {
        if (event->mimeData()->hasUrls())
        {
            for (const auto& url : event->mimeData()->urls())
            {
                if (url.isLocalFile())
                {
                    event->acceptProposedAction();
                    return;
                }
            }
        }

        QWidget::dragMoveEvent(event);
    }

    void ComposeTabWidget::dropEvent(QDropEvent* event)
    {
        QStringList filePaths;
        for (const auto& url : event->mimeData()->urls())
        {
            if (url.isLocalFile())
            {
                filePaths.push_back(url.toLocalFile());
            }
        }

        if (filePaths.empty())
        {
            QWidget::dropEvent(event);
            return;
        }

        addAttachmentPaths(filePaths);
        event->acceptProposedAction();
    }

    void ComposeTabWidget::setupUi()
    {
        auto* rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(4);

        auto* headerWidget = new QWidget(this);
        auto* headerLayout = new QVBoxLayout(headerWidget);
        headerLayout->setContentsMargins(6, 6, 6, 0);
        headerLayout->setSpacing(4);

        auto* fromRow = new QHBoxLayout();
        m_fromLabel = new QLabel(i18nc("@label email sender", "From"), headerWidget);
        m_fromCombo = new QComboBox(headerWidget);
        m_fromLabel->setBuddy(m_fromCombo);
        fromRow->addWidget(m_fromLabel);
        fromRow->addWidget(m_fromCombo, 1);
        headerLayout->addLayout(fromRow);

        m_signatureMenu = new QMenu(this);
        connect(m_signatureMenu, &QMenu::aboutToShow, this,
                &ComposeTabWidget::refreshSignatureMenu);

        auto* recipientRowsLayout = new QVBoxLayout();
        recipientRowsLayout->setContentsMargins(0, 0, 0, 0);
        recipientRowsLayout->setSpacing(4);
        headerLayout->addLayout(recipientRowsLayout);
        m_recipientController = std::make_unique<ComposeRecipientController>(
            *recipientRowsLayout, *this, [this] { scheduleWorkingCopySave(); });

        auto* subjectRow = new QHBoxLayout();
        m_subjectLabel = new QLabel(i18nc("@label email subject", "Subject"), headerWidget);
        m_subjectEdit = new QLineEdit(headerWidget);
        m_subjectLabel->setBuddy(m_subjectEdit);
        m_subjectEdit->setPlaceholderText(i18n("Add a subject"));
        subjectRow->addWidget(m_subjectLabel);
        subjectRow->addWidget(m_subjectEdit, 1);
        headerLayout->addLayout(subjectRow);
        m_recipientController->updateLabelWidths(*m_fromLabel, *m_subjectLabel);

        rootLayout->addWidget(headerWidget);

        m_formatToolbar = new QToolBar(this);
        m_formatToolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        rootLayout->addWidget(m_formatToolbar);

        m_editorTabs = new QTabWidget(this);
        m_richTextEdit = new JavelinComposerEdit(m_editorTabs);
        m_richTextEdit->setAccessibleName(
            i18nc("@label accessible email composer", "Message body"));
        m_richTextEdit->setAcceptDrops(false);
        m_richTextEdit->setAcceptRichText(true);
        m_richTextEdit->document()->setDocumentMargin(8);
        m_previewView = new javelin::gui::messageview::HtmlMessageView(
            m_settings.messageAppearanceSettings(), m_editorTabs);
        m_previewView->setAccessibleName(
            i18nc("@label accessible email preview", "Message preview"));
        m_previewView->setAcceptDrops(false);
        m_previewView->setRemoteContentEnabled(false);
        m_editorTabs->addTab(m_richTextEdit, i18n("Compose"));
        m_editorTabs->addTab(m_previewView, i18n("Preview"));
        rootLayout->addWidget(m_editorTabs, 1);

        m_attachmentScrollArea = new QScrollArea(this);
        m_attachmentScrollArea->setFrameShape(QFrame::NoFrame);
        m_attachmentScrollArea->setWidgetResizable(true);
        m_attachmentScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_attachmentScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_attachmentScrollArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_attachmentStrip = new QWidget(m_attachmentScrollArea);
        m_attachmentStripLayout = new QHBoxLayout(m_attachmentStrip);
        m_attachmentStripLayout->setContentsMargins(0, 0, 0, 0);
        m_attachmentStripLayout->setSpacing(6);
        m_attachmentStripLayout->addStretch(1);
        m_attachmentScrollArea->setWidget(m_attachmentStrip);
        m_attachmentScrollArea->setFixedHeight(fontMetrics().height() + 24);
        rootLayout->addWidget(m_attachmentScrollArea);
    }

    void ComposeTabWidget::createToolbarActions()
    {
        m_actionCollection = new KActionCollection(this, QStringLiteral("javelin-composer"));
        m_actionCollection->addAssociatedWidget(this);
        m_richTextEdit->createActions(m_actionCollection);

        const auto addKdeAction = [this](const QString& name) -> QAction*
        {
            auto* action = m_actionCollection->action(name);
            if (action != nullptr)
            {
                m_formatToolbar->addAction(action);
            }
            return action;
        };

        addKdeAction(QStringLiteral("format_heading_level"));
        addKdeAction(QStringLiteral("format_list_style"));
        addKdeAction(QStringLiteral("format_font_family"));
        addKdeAction(QStringLiteral("format_font_size"));
        m_formatToolbar->addSeparator();

        addKdeAction(QStringLiteral("format_text_bold"));
        addKdeAction(QStringLiteral("format_text_italic"));
        addKdeAction(QStringLiteral("format_text_underline"));
        addKdeAction(QStringLiteral("format_text_strikeout"));

        m_codeAction = new QAction(QIcon::fromTheme(QStringLiteral("format-text-code")),
                                   i18nc("@action text formatting", "Code"), this);
        m_codeAction->setCheckable(true);
        m_actionCollection->addAction(QStringLiteral("javelin_format_code"), m_codeAction);
        m_formatToolbar->addAction(m_codeAction);
        connect(m_codeAction, &QAction::triggered, this, &ComposeTabWidget::toggleCode);

        addKdeAction(QStringLiteral("format_text_foreground_color"));
        addKdeAction(QStringLiteral("format_text_background_color"));
        m_formatToolbar->addSeparator();

        addKdeAction(QStringLiteral("format_align_left"));
        addKdeAction(QStringLiteral("format_align_center"));
        addKdeAction(QStringLiteral("format_align_right"));
        addKdeAction(QStringLiteral("format_align_justify"));
        addKdeAction(QStringLiteral("format_list_indent_more"));
        addKdeAction(QStringLiteral("format_list_indent_less"));
        m_formatToolbar->addSeparator();

        addKdeAction(QStringLiteral("manage_link"));
        addKdeAction(QStringLiteral("insert_horizontal_rule"));

        m_insertImageAction = new QAction(QIcon::fromTheme(QStringLiteral("insert-image")),
                                          i18nc("@action insert image", "Image"), this);
        m_actionCollection->addAction(QStringLiteral("javelin_insert_image"), m_insertImageAction);
        m_formatToolbar->addAction(m_insertImageAction);
        connect(m_insertImageAction, &QAction::triggered, this, &ComposeTabWidget::insertImage);

        addKdeAction(QStringLiteral("insert_html"));
        addKdeAction(QStringLiteral("insert_table"));
        addKdeAction(QStringLiteral("format_list_checkbox"));
        addKdeAction(QStringLiteral("format_reset"));
        addKdeAction(QStringLiteral("format_painter"));
        addKdeAction(QStringLiteral("direction_ltr"));
        addKdeAction(QStringLiteral("direction_rtl"));
    }

    void ComposeTabWidget::loadIdentities()
    {
        m_identityController->load(m_snapshot.accountId, m_snapshot.identityId);
        m_previousIdentityIndex = m_fromCombo->currentIndex();
    }

    void ComposeTabWidget::reloadSenderIdentities(const QString& changedAccountId)
    {
        const auto selectedAccountId = m_fromCombo->currentData(senderAccountIdRole).toString();
        const bool replaceNativeSignature =
            (changedAccountId.isEmpty() || changedAccountId == selectedAccountId) &&
            m_signatureController->shouldRestoreAfterIdentityReload();
        loadIdentities();
        const auto selectedIdentityStillAvailable =
            m_fromCombo->currentIndex() >= 0 &&
            m_fromCombo->currentData(senderAccountIdRole).toString().toStdString() ==
                m_snapshot.accountId &&
            m_fromCombo->currentData(senderIdentityIdRole).toString().toStdString() ==
                m_snapshot.identityId;
        if (replaceNativeSignature && selectedIdentityStillAvailable)
            replaceTrackedSignatureForIndex(m_fromCombo->currentIndex(), true);
        Q_EMIT toolbarStateChanged();
    }

    void ComposeTabWidget::initializeSignatureTracking()
    {
        m_signatureController->initialize();
    }

    void ComposeTabWidget::replaceTrackedSignatureForIndex(const int index, const bool forceInsert)
    {
        m_signatureController->replaceForIdentity(index, forceInsert);
    }

    void ComposeTabWidget::removeTrackedSignature()
    {
        m_signatureController->remove();
    }

    void ComposeTabWidget::refreshSignatureMenu()
    {
        m_signatureMenu->clear();
        auto* useIdentity = m_signatureMenu->addAction(i18n("Use Identity Signature"));
        connect(useIdentity, &QAction::triggered, this,
                [this] { m_signatureController->restoreIdentity(m_fromCombo->currentIndex()); });
        auto* noSignature = m_signatureMenu->addAction(i18n("No Signature for This Message"));
        connect(noSignature, &QAction::triggered, this, &ComposeTabWidget::removeTrackedSignature);

        const auto email = m_fromCombo->currentData(senderEmailRole).toString();
        if (!email.isEmpty())
        {
            auto* variants = m_signatureMenu->addMenu(i18n("Signature Variant"));
            for (int index = 0; index < m_fromCombo->count(); ++index)
            {
                if (m_fromCombo->itemData(index, senderEmailRole)
                        .toString()
                        .compare(email, Qt::CaseInsensitive) != 0)
                    continue;
                auto* action = variants->addAction(m_fromCombo->itemText(index));
                action->setCheckable(true);
                action->setChecked(index == m_fromCombo->currentIndex());
                connect(action, &QAction::triggered, this,
                        [this, index] { m_fromCombo->setCurrentIndex(index); });
            }
        }

        m_signatureMenu->addSeparator();
        auto* editCurrent = m_signatureMenu->addAction(i18n("Edit Current Signature…"));
        editCurrent->setEnabled(m_fromCombo->currentIndex() >= 0);
        connect(editCurrent, &QAction::triggered, this, &ComposeTabWidget::editCurrentSignature);
        auto* manage = m_signatureMenu->addAction(i18n("Manage Identities and Signatures…"));
        connect(manage, &QAction::triggered, this, &ComposeTabWidget::editCurrentSignature);
    }

    void ComposeTabWidget::applySnapshotToUi()
    {
        m_syncingUi = true;
        const QSignalBlocker fromBlocker{m_fromCombo};
        const QSignalBlocker subjectBlocker{m_subjectEdit};
        const QSignalBlocker richBlocker{m_richTextEdit};
        const QSignalBlocker tabBlocker{m_editorTabs};

        int identityIndex = -1;
        for (int index = 0; index < m_fromCombo->count(); ++index)
        {
            if (m_fromCombo->itemData(index, senderAccountIdRole).toString().toStdString() ==
                    m_snapshot.accountId &&
                m_fromCombo->itemData(index, senderIdentityIdRole).toString().toStdString() ==
                    m_snapshot.identityId)
            {
                identityIndex = index;
                break;
            }
        }
        if (identityIndex >= 0)
        {
            m_fromCombo->setCurrentIndex(identityIndex);
        }
        m_previousIdentityIndex = m_fromCombo->currentIndex();

        m_recipientController->setSyncing(true);
        m_recipientController->reset(m_snapshot);
        m_recipientController->setSyncing(false);
        m_recipientController->updateLabelWidths(*m_fromLabel, *m_subjectLabel);
        m_subjectEdit->setText(m_snapshot.subject.has_value()
                                   ? QString::fromStdString(*m_snapshot.subject)
                                   : QString{});

        if (m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::RawHtml)
        {
            m_snapshot.editorMode = javelin::jmap::submission::BodyEditorMode::RichText;
        }
        const bool plainTextMode =
            m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::PlainText;
        if (plainTextMode)
        {
            m_richTextEdit->setPlainText(QString::fromStdString(m_snapshot.plainTextBody));
            m_richTextEdit->forcePlainTextMarkup(false);
            m_richTextEdit->switchToPlainText();
        }
        else
        {
            setEditorHtml(QString::fromStdString(m_snapshot.htmlBody));
        }
        m_editorTabs->setTabVisible(previewTabIndex, !plainTextMode);
        m_editorTabs->setCurrentIndex(richEditorTabIndex);
        populateAttachments();
        m_syncingUi = false;
        initializeSignatureTracking();
        QTextCursor cursor{m_richTextEdit->document()};
        cursor.setPosition(0);
        m_richTextEdit->setTextCursor(cursor);
    }

    void ComposeTabWidget::populateAttachments()
    {
        m_attachmentController->refresh(m_operationInFlight);
    }

    void ComposeTabWidget::refreshPreview()
    {
        if (m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::PlainText)
        {
            m_previewView->clearDocument();
            return;
        }
        m_previewView->setDocumentHtml(stableEditorHtml().toStdString());
    }

    void ComposeTabWidget::syncSnapshotFromUi()
    {
        const auto selectedAccountId =
            m_fromCombo->currentData(senderAccountIdRole).toString().toStdString();
        const auto selectedIdentityId =
            m_fromCombo->currentData(senderIdentityIdRole).toString().toStdString();
        if (!selectedAccountId.empty() && !selectedIdentityId.empty())
        {
            const bool selectedAccountChanged = selectedAccountId != m_snapshot.accountId;
            m_snapshot.accountId = selectedAccountId;
            m_snapshot.identityId = selectedIdentityId;
            if (selectedAccountChanged)
            {
                m_snapshot.draftEmailId.reset();
                Q_EMIT accountChanged(QString::fromStdString(m_snapshot.accountId));
            }
        }
        m_snapshot.to = recipientAddresses(RecipientType::To);
        m_snapshot.cc = recipientAddresses(RecipientType::Cc);
        m_snapshot.bcc = recipientAddresses(RecipientType::Bcc);
        m_snapshot.subject =
            m_subjectEdit->text().trimmed().isEmpty()
                ? std::nullopt
                : std::optional<std::string>{m_subjectEdit->text().trimmed().toStdString()};

        switch (m_snapshot.editorMode)
        {
        case javelin::jmap::submission::BodyEditorMode::RawHtml:
            m_snapshot.editorMode = javelin::jmap::submission::BodyEditorMode::RichText;
            [[fallthrough]];
        case javelin::jmap::submission::BodyEditorMode::RichText:
        {
            const auto html = stableEditorHtml();
            reconcileInlineAttachmentReferences(html);
            m_snapshot.htmlBody = html.toStdString();
            m_snapshot.plainTextBody = m_richTextEdit->toCleanPlainText().toStdString();
            break;
        }
        case javelin::jmap::submission::BodyEditorMode::PlainText:
            m_snapshot.plainTextBody = m_richTextEdit->toCleanPlainText().toStdString();
            m_snapshot.htmlBody.clear();
            break;
        }

        ++m_snapshot.revision;
        updateTabTitle();
    }

    void ComposeTabWidget::switchBodyFormat(const bool richText)
    {
        if (m_syncingUi)
        {
            return;
        }

        const bool plainTextMode = !richText;
        const bool restoreNativeSignature =
            m_signatureController->shouldRestoreAfterIdentityReload();
        const int selectedIdentityIndex = m_fromCombo->currentIndex();
        const auto removeNativeSignature = [this, restoreNativeSignature]
        {
            if (restoreNativeSignature)
                (void)m_signatureController->detachForBodyFormatSwitch();
        };
        if (plainTextMode &&
            m_snapshot.editorMode != javelin::jmap::submission::BodyEditorMode::PlainText)
        {
            bool useMarkup = false;
            if (m_richTextEdit->composerControler()->isFormattingUsed())
            {
                QMessageBox warning{
                    QMessageBox::Warning, i18n("Convert to Plain Text"),
                    i18n("This message contains formatting. How should it be converted to plain "
                         "text?"),
                    QMessageBox::NoButton, this};
                warning.setInformativeText(confirmationDetails());
                QAbstractButton* loseFormatting = warning.addButton(
                    i18nc("@action:button", "Lose Formatting"), QMessageBox::DestructiveRole);
                QPushButton* addMarkup = warning.addButton(
                    i18nc("@action:button", "Add Markup Plain Text"), QMessageBox::AcceptRole);
                QAbstractButton* cancel = warning.addButton(QMessageBox::Cancel);
                warning.setDefaultButton(addMarkup);
                warning.exec();

                if (warning.clickedButton() == cancel || warning.clickedButton() == nullptr)
                {
                    Q_EMIT toolbarStateChanged();
                    return;
                }
                useMarkup = warning.clickedButton() == addMarkup;
                Q_UNUSED(loseFormatting);
            }

            removeNativeSignature();
            m_syncingUi = true;
            m_richTextEdit->forcePlainTextMarkup(useMarkup);
            m_richTextEdit->switchToPlainText();
            m_snapshot.editorMode = javelin::jmap::submission::BodyEditorMode::PlainText;
            m_syncingUi = false;
        }
        else if (richText &&
                 m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::PlainText)
        {
            removeNativeSignature();
            m_syncingUi = true;
            m_richTextEdit->activateRichText();
            m_snapshot.editorMode = javelin::jmap::submission::BodyEditorMode::RichText;
            m_syncingUi = false;
        }
        if (restoreNativeSignature)
            replaceTrackedSignatureForIndex(selectedIdentityIndex, true);

        if (const auto error = ComposeUiPreferences::setRichTextDefault(m_settings, richText))
        {
            Q_EMIT statusMessageRequested(
                i18n("Could not remember the compose format: %1", error->detail), 10000);
        }
        m_editorTabs->setCurrentIndex(richEditorTabIndex);
        m_editorTabs->setTabVisible(previewTabIndex, !plainTextMode);

        Q_EMIT toolbarStateChanged();
        populateAttachments();
        updateEditorModeUi();
        syncSnapshotFromUi();
        refreshPreview();
        scheduleWorkingCopySave();
    }

    void ComposeTabWidget::setRecipientText(const RecipientType type, const QString& text)
    {
        m_recipientController->setText(type, text);
    }

    QString ComposeTabWidget::recipientText(const RecipientType type) const
    {
        return m_recipientController->text(type);
    }

    std::vector<javelin::jmap::domain::EmailAddress>
    ComposeTabWidget::recipientAddresses(const RecipientType type) const
    {
        return m_recipientController->addresses(type);
    }

    void ComposeTabWidget::scheduleWorkingCopySave()
    {
        m_autosaveController->schedule();
    }

    void ComposeTabWidget::persistWorkingCopy()
    {
        if (m_inlineImageController->hasPendingJobs())
            return;

        syncSnapshotFromUi();
        if (const auto error = m_composeCommandPort.storeWorkingCopy(m_snapshot))
        {
            Q_EMIT statusMessageRequested(error->message, 10000);
            return;
        }
    }

    void ComposeTabWidget::setBusy(const bool busy)
    {
        m_operationInFlight = busy;
        m_autosaveController->setBusy(busy);
        m_fromCombo->setEnabled(!busy);
        m_recipientController->setEnabled(!busy);
        m_subjectEdit->setEnabled(!busy);
        m_richTextEdit->setEnabled(!busy);
        m_editorTabs->setEnabled(!busy);
        updateEditorModeUi();
        populateAttachments();
        Q_EMIT toolbarStateChanged();
    }

    void ComposeTabWidget::updateEditorModeUi()
    {
        const bool richMode =
            m_editorTabs->currentIndex() == richEditorTabIndex &&
            m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::RichText;
        const bool actionsEnabled = !m_operationInFlight && richMode;
        m_formatToolbar->setVisible(m_snapshot.editorMode ==
                                    javelin::jmap::submission::BodyEditorMode::RichText);
        m_formatToolbar->setEnabled(!m_operationInFlight);
        m_richTextEdit->setEnableActions(actionsEnabled);
        m_codeAction->setEnabled(actionsEnabled);
        m_insertImageAction->setEnabled(actionsEnabled);
    }

    void ComposeTabWidget::updateTabTitle()
    {
        Q_EMIT titleChanged(tabTitle());
    }

    void ComposeTabWidget::addAttachments()
    {
        const auto filePaths = QFileDialog::getOpenFileNames(this, i18n("Attach Files"));
        addAttachmentPaths(filePaths);
    }

    void ComposeTabWidget::attachFiles()
    {
        if (!m_operationInFlight)
            addAttachments();
    }

    void ComposeTabWidget::saveDraft()
    {
        startSaveDraft(false);
    }

    void ComposeTabWidget::sendMessage()
    {
        startSend();
    }

    void ComposeTabWidget::scheduleMessage()
    {
        if (m_operationInFlight)
            return;
        if (!canSend())
        {
            Q_EMIT statusMessageRequested(
                i18n("Choose an available sender identity before scheduling."), 10000);
            return;
        }
        const auto maxDelayedSend = currentMaxDelayedSendSeconds();
        if (!maxDelayedSend.has_value())
        {
            Q_EMIT statusMessageRequested(
                i18n("This sender's server does not support scheduled sending."), 10000);
            return;
        }
        if (m_inlineImageController->hasPendingJobs())
        {
            m_deferredOperation = DeferredOperation::ScheduleSend;
            Q_EMIT statusMessageRequested(i18n("Finishing image processing before scheduling…"),
                                          10000);
            return;
        }

        const auto now = QDateTime::currentDateTime();
        const auto maximumSeconds = static_cast<qint64>(std::min<std::uint64_t>(
            *maxDelayedSend, static_cast<std::uint64_t>(std::numeric_limits<qint64>::max())));
        QDialog dialog{this};
        dialog.setWindowTitle(i18n("Schedule Send"));
        auto* layout = new QFormLayout(&dialog);
        auto* sendAtEdit = new QDateTimeEdit(now.addSecs(3600), &dialog);
        sendAtEdit->setCalendarPopup(true);
        sendAtEdit->setMinimumDateTime(now.addSecs(1));
        sendAtEdit->setMaximumDateTime(now.addSecs(maximumSeconds));
        sendAtEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
        layout->addRow(i18n("Send at:"), sendAtEdit);
        layout->addRow(QString{}, new QLabel(i18n("Times are shown in %1.",
                                                  QString::fromUtf8(QTimeZone::systemTimeZoneId())),
                                             &dialog));
        auto* buttons =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        buttons->button(QDialogButtonBox::Ok)->setText(i18n("Schedule"));
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addRow(buttons);
        if (dialog.exec() != QDialog::Accepted)
            return;

        const auto selectedSeconds = sendAtEdit->dateTime().toSecsSinceEpoch();
        startSend(std::chrono::system_clock::time_point{std::chrono::seconds{selectedSeconds}});
    }

    void ComposeTabWidget::addAttachmentPaths(const QStringList& filePaths)
    {
        for (const auto& filePath : filePaths)
        {
            const QFileInfo info{filePath};
            if (!info.exists() || !info.isFile())
            {
                continue;
            }

            m_snapshot.attachments.push_back(javelin::jmap::submission::DraftAttachment{
                .localFilePath = filePath.toStdString(),
                .displayName = info.fileName().toStdString(),
                .mediaType = detectedMediaType(filePath).toStdString(),
                .size = static_cast<std::uint64_t>(info.size()),
                .blobId = std::nullopt,
                .inlineDisposition = false,
                .contentId = std::nullopt,
                .contentHash = std::nullopt,
            });
        }

        populateAttachments();
        scheduleWorkingCopySave();
    }

    void ComposeTabWidget::addPastedInlineImage(const QImage& image)
    {
        m_inlineImageController->addPastedImage(image);
    }

    void ComposeTabWidget::insertImage()
    {
        const auto insertionPosition = m_richTextEdit->textCursor().selectionStart();
        auto* document = m_richTextEdit->document();
        const auto documentRevision = document->revision();
        auto timing = std::make_shared<ImageInsertTiming>();
        timing->elapsed.start();

        qCInfo(logComposeImage).noquote()
            << "insert dialog opened"
            << "documentRevision" << documentRevision << "insertionPosition" << insertionPosition;

        timing->documentChangeConnection =
            connect(document, &QTextDocument::contentsChange, this,
                    [timing](const int position, const int removed, const int added)
                    {
                        const auto elapsedMilliseconds = timing->elapsed.elapsed();
                        const auto postAcceptMilliseconds =
                            timing->acceptedAtMilliseconds.has_value()
                                ? elapsedMilliseconds - *timing->acceptedAtMilliseconds
                                : -1;
                        qCInfo(logComposeImage).noquote()
                            << "editor document changed"
                            << "elapsedMs" << elapsedMilliseconds << "postAcceptMs"
                            << postAcceptMilliseconds << "position" << position << "removed"
                            << removed << "added" << added;
                        QObject::disconnect(timing->documentChangeConnection);
                    });

        QTimer::singleShot(
            0, this,
            [this, timing]
            {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (dialog == nullptr)
                {
                    qCWarning(logComposeImage)
                        << "insert dialog instrumentation could not find the active modal dialog";
                    return;
                }
                connect(
                    dialog, &QDialog::accepted, this,
                    [timing, dialog]
                    {
                        timing->acceptedAtMilliseconds = timing->elapsed.elapsed();
                        timing->sourceFilePath = imageDialogSourceFilePath(*dialog);
                        qCInfo(logComposeImage).noquote()
                            << "insert dialog accepted"
                            << "elapsedMs" << *timing->acceptedAtMilliseconds << "sourceFile"
                            << timing->sourceFilePath;
                    },
                    Qt::SingleShotConnection);
            });

        m_richTextEdit->composerControler()->slotAddImage();
        QObject::disconnect(timing->documentChangeConnection);

        const auto elapsedMilliseconds = timing->elapsed.elapsed();
        const auto postAcceptMilliseconds =
            timing->acceptedAtMilliseconds.has_value()
                ? elapsedMilliseconds - *timing->acceptedAtMilliseconds
                : -1;
        const bool documentChanged = document->revision() != documentRevision;
        qCInfo(logComposeImage).noquote()
            << "KDE image insertion returned"
            << "elapsedMs" << elapsedMilliseconds << "postAcceptMs" << postAcceptMilliseconds
            << "documentChanged" << documentChanged << "documentRevision" << document->revision();

        if (documentChanged)
        {
            adoptInsertedComposerImage(insertionPosition, timing->sourceFilePath);
        }
    }

    void ComposeTabWidget::adoptInsertedComposerImage(const int insertionPosition,
                                                      const QString& sourceFilePath)
    {
        m_inlineImageController->adoptInsertedComposerImage(insertionPosition, sourceFilePath);
    }

    void ComposeTabWidget::finishInlineImagePreparation(const bool succeeded)
    {
        scheduleWorkingCopySave();
        auto deferredOperation = std::exchange(m_deferredOperation, DeferredOperation::None);
        if (!succeeded && deferredOperation == DeferredOperation::Send)
            deferredOperation = DeferredOperation::None;
        QTimer::singleShot(0, this,
                           [this, deferredOperation]
                           {
                               switch (deferredOperation)
                               {
                               case DeferredOperation::None:
                                   break;
                               case DeferredOperation::SaveDraft:
                                   startSaveDraft(false);
                                   break;
                               case DeferredOperation::SaveDraftAndClose:
                                   startSaveDraft(true);
                                   break;
                               case DeferredOperation::Send:
                                   startSend();
                                   break;
                               case DeferredOperation::ScheduleSend:
                                   scheduleMessage();
                                   break;
                               }
                           });
    }

    void ComposeTabWidget::removeAttachmentAt(const std::size_t index)
    {
        if (index >= m_snapshot.attachments.size())
        {
            return;
        }

        if (m_snapshot.attachments[index].contentId.has_value())
        {
            removeEmbeddedImageReference(*m_snapshot.attachments[index].contentId);
        }
        m_snapshot.attachments.erase(m_snapshot.attachments.begin() +
                                     static_cast<std::ptrdiff_t>(index));
        populateAttachments();
        refreshPreview();
        syncSnapshotFromUi();
        scheduleWorkingCopySave();
    }

    void ComposeTabWidget::setAttachmentEmbedded(const std::size_t index, const bool embedded)
    {
        if (!m_inlineImageController->setAttachmentEmbedded(index, embedded))
            return;
        refreshPreview();
        syncSnapshotFromUi();
        scheduleWorkingCopySave();
    }

    void ComposeTabWidget::insertEmbeddedImage(const std::size_t index)
    {
        m_inlineImageController->insertEmbeddedImage(index);
    }

    void ComposeTabWidget::removeEmbeddedImageReference(const std::string& contentId)
    {
        m_inlineImageController->removeEmbeddedImageReference(contentId);
    }

    void ComposeTabWidget::setEditorHtml(const QString& html)
    {
        m_inlineImageController->setEditorHtml(html);
    }

    QString ComposeTabWidget::stableEditorHtml()
    {
        return m_inlineImageController->stableHtml();
    }

    void ComposeTabWidget::reconcileInlineAttachmentReferences(const QString& html)
    {
        m_inlineImageController->reconcileAttachmentReferences(html);
    }

    void ComposeTabWidget::startSaveDraft(const bool closeAfterSave)
    {
        if (m_operationInFlight)
        {
            return;
        }
        if (m_inlineImageController->hasPendingJobs())
        {
            m_deferredOperation = closeAfterSave ? DeferredOperation::SaveDraftAndClose
                                                 : DeferredOperation::SaveDraft;
            Q_EMIT statusMessageRequested(i18n("Finishing image processing before saving…"), 10000);
            return;
        }

        syncSnapshotFromUi();
        QString errorMessage;
        const auto settings = liveSettings(m_settings, m_snapshot.accountId, &errorMessage);
        if (!settings.has_value())
        {
            Q_EMIT statusMessageRequested(errorMessage, 10000);
            Q_EMIT userInterventionRequired(errorMessage);
            return;
        }

        if (const auto error = m_composeCommandPort.storeWorkingCopy(m_snapshot))
        {
            Q_EMIT statusMessageRequested(error->message, 10000);
            return;
        }

        setBusy(true);
        m_closeAfterSave = closeAfterSave;
        Q_EMIT statusMessageRequested(i18n("Saving draft..."), 5000);
        auto task = m_composeCommandPort.saveDraft(*settings, m_snapshot);
        QCoro::connect(
            std::move(task), this,
            [this](std::variant<javelin::jmap::submission::DraftSaveSummary,
                                javelin::jmap::OperationError>
                       result)
            {
                setBusy(false);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    Q_EMIT statusMessageRequested(error->message, 10000);
                    m_closeAfterSave = false;
                    return;
                }

                const auto& summary = std::get<javelin::jmap::submission::DraftSaveSummary>(result);
                m_snapshot = summary.savedSnapshot;
                populateAttachments();
                if (const auto error = m_composeCommandPort.storeWorkingCopy(m_snapshot))
                {
                    Q_EMIT statusMessageRequested(error->message, 10000);
                    m_closeAfterSave = false;
                    return;
                }

                m_autosaveController->markSaved();
                Q_EMIT statusMessageRequested(i18n("Draft saved."), 5000);
                if (m_closeAfterSave)
                {
                    m_closeAfterSave = false;
                    if (const auto error =
                            m_composeCommandPort.discard(m_snapshot.composeSessionId))
                    {
                        Q_EMIT statusMessageRequested(error->message, 10000);
                        return;
                    }
                    m_closeWithoutPrompt = true;
                    Q_EMIT closeRequested();
                }
            });
    }

    void
    ComposeTabWidget::startSend(const std::optional<std::chrono::system_clock::time_point> sendAt)
    {
        if (m_operationInFlight)
        {
            return;
        }
        if (!canSend())
        {
            Q_EMIT statusMessageRequested(
                i18n("Choose an available sender identity before sending."), 10000);
            return;
        }
        if (m_inlineImageController->hasPendingJobs())
        {
            m_deferredOperation = DeferredOperation::Send;
            Q_EMIT statusMessageRequested(i18n("Finishing image processing before sending…"),
                                          10000);
            return;
        }

        syncSnapshotFromUi();
        QString errorMessage;
        const auto settings = liveSettings(m_settings, m_snapshot.accountId, &errorMessage);
        if (!settings.has_value())
        {
            Q_EMIT statusMessageRequested(errorMessage, 10000);
            Q_EMIT userInterventionRequired(errorMessage);
            return;
        }

        if (m_snapshot.to.empty() && m_snapshot.cc.empty() && m_snapshot.bcc.empty())
        {
            Q_EMIT statusMessageRequested(i18n("Add at least one recipient before sending."), 7000);
            return;
        }

        if (!m_snapshot.subject.has_value())
        {
            QMessageBox messageBox{this};
            messageBox.setWindowTitle(i18n("Send Without Subject?"));
            messageBox.setText(i18n("This message has no subject. Send it anyway?"));
            messageBox.setInformativeText(confirmationDetails());
            QAbstractButton* sendAnywayButton =
                messageBox.addButton(i18n("Send Anyway"), QMessageBox::AcceptRole);
            messageBox.addButton(QMessageBox::Cancel);
            messageBox.exec();
            if (messageBox.clickedButton() != sendAnywayButton)
            {
                m_subjectEdit->setFocus();
                return;
            }
        }

        setBusy(true);
        Q_EMIT statusMessageRequested(
            sendAt.has_value() ? i18n("Scheduling message…") : i18n("Sending message..."), 5000);
        QCoro::Task<
            std::variant<javelin::jmap::submission::SendSummary, javelin::jmap::OperationError>>
            task = sendAt.has_value() ? m_composeCommandPort.scheduleSend(
                                            *settings, {.snapshot = m_snapshot, .sendAt = *sendAt})
                                      : m_composeCommandPort.send(*settings, m_snapshot);
        QCoro::connect(
            std::move(task), this,
            [this](
                std::variant<javelin::jmap::submission::SendSummary, javelin::jmap::OperationError>
                    result)
            {
                setBusy(false);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    Q_EMIT statusMessageRequested(error->message, 10000);
                    return;
                }

                const auto& summary = std::get<javelin::jmap::submission::SendSummary>(result);
                m_snapshot.draftEmailId = summary.draftEmailId;
                m_closeWithoutPrompt = true;
                Q_EMIT statusMessageRequested(
                    summary.scheduled ? i18n("Message scheduled.") : i18n("Message sent."), 7000);
                Q_EMIT closeRequested();
            });
    }

    void ComposeTabWidget::toggleCode()
    {
        QTextCharFormat format;
        if (m_codeAction->isChecked())
        {
            format.setFontFamilies(QStringList{QStringLiteral("monospace")});
        }
        else
        {
            format.clearProperty(QTextFormat::FontFamilies);
        }
        m_richTextEdit->mergeCurrentCharFormat(format);
    }

} // namespace javelin::gui::compose
