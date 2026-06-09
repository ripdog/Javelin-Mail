#include "gui/compose/ComposeTabWidget.h"

#include "gui/messageview/HtmlMessageView.h"
#include "gui/settings/PreferencesDialog.h"
#include "jmap/cache/IdentityRepository.h"
#include "jmap/submission/ComposeService.h"

#include <QCoroTask>

#include <QAction>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextListFormat>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>

namespace javelin::gui::compose
{

    namespace
    {

        constexpr auto richEditorTabIndex = 0;
        constexpr auto htmlSourceTabIndex = 1;
        constexpr auto previewTabIndex = 2;

        [[nodiscard]] QString defaultTitleForMode(
            const javelin::jmap::submission::ComposeMode mode)
        {
            switch (mode)
            {
            case javelin::jmap::submission::ComposeMode::NewMessage:
                return QStringLiteral("New Message");
            case javelin::jmap::submission::ComposeMode::Reply:
                return QStringLiteral("Reply");
            case javelin::jmap::submission::ComposeMode::ReplyAll:
                return QStringLiteral("Reply All");
            case javelin::jmap::submission::ComposeMode::Forward:
                return QStringLiteral("Forward");
            case javelin::jmap::submission::ComposeMode::EditDraft:
                return QStringLiteral("Edit Draft");
            }

            return QStringLiteral("Compose");
        }

        [[nodiscard]] QString displayAddress(
            const javelin::jmap::domain::EmailAddress& address)
        {
            if (address.name.has_value() && !address.name->empty())
            {
                return QStringLiteral("%1 <%2>")
                    .arg(QString::fromStdString(*address.name),
                         QString::fromStdString(address.email));
            }

            return QString::fromStdString(address.email);
        }

        [[nodiscard]] QString formatAddresses(
            const std::vector<javelin::jmap::domain::EmailAddress>& addresses)
        {
            QStringList parts;
            for (const auto& address : addresses)
            {
                parts.push_back(displayAddress(address));
            }
            return parts.join(QStringLiteral(", "));
        }

        [[nodiscard]] std::optional<javelin::jmap::domain::EmailAddress>
        parseAddressToken(const QString& token)
        {
            const auto trimmed = token.trimmed();
            if (trimmed.isEmpty())
            {
                return std::nullopt;
            }

            const auto openBracket = trimmed.lastIndexOf(QLatin1Char('<'));
            const auto closeBracket = trimmed.lastIndexOf(QLatin1Char('>'));
            if (openBracket >= 0 && closeBracket > openBracket)
            {
                const auto name =
                    trimmed.left(openBracket).trimmed().remove(QLatin1Char('"'));
                const auto email =
                    trimmed.mid(openBracket + 1, closeBracket - openBracket - 1).trimmed();
                if (!email.contains(QLatin1Char('@')))
                {
                    return std::nullopt;
                }
                return javelin::jmap::domain::EmailAddress{
                    .name = name.isEmpty() ? std::nullopt
                                           : std::optional<std::string>{name.toStdString()},
                    .email = email.toStdString(),
                };
            }

            if (!trimmed.contains(QLatin1Char('@')))
            {
                return std::nullopt;
            }

            return javelin::jmap::domain::EmailAddress{
                .name = std::nullopt,
                .email = trimmed.toStdString(),
            };
        }

        [[nodiscard]] std::vector<javelin::jmap::domain::EmailAddress>
        parseAddresses(const QString& value)
        {
            std::vector<javelin::jmap::domain::EmailAddress> addresses;
            QString current;
            bool insideAngleBrackets = false;
            for (const auto character : value)
            {
                if (character == QLatin1Char('<'))
                {
                    insideAngleBrackets = true;
                }
                else if (character == QLatin1Char('>'))
                {
                    insideAngleBrackets = false;
                }

                if (!insideAngleBrackets &&
                    (character == QLatin1Char(',') || character == QLatin1Char(';')))
                {
                    if (const auto parsed = parseAddressToken(current))
                    {
                        addresses.push_back(*parsed);
                    }
                    current.clear();
                    continue;
                }

                current.append(character);
            }

            if (const auto parsed = parseAddressToken(current))
            {
                addresses.push_back(*parsed);
            }

            return addresses;
        }

        [[nodiscard]] QString attachmentSizeLabel(const std::uint64_t size)
        {
            constexpr double kib = 1024.0;
            constexpr double mib = 1024.0 * kib;
            constexpr double gib = 1024.0 * mib;

            if (size >= static_cast<std::uint64_t>(gib))
            {
                return QStringLiteral("%1 GB").arg(static_cast<double>(size) / gib, 0, 'f', 1);
            }
            if (size >= static_cast<std::uint64_t>(mib))
            {
                return QStringLiteral("%1 MB").arg(static_cast<double>(size) / mib, 0, 'f', 1);
            }
            if (size >= static_cast<std::uint64_t>(kib))
            {
                return QStringLiteral("%1 KB").arg(static_cast<double>(size) / kib, 0, 'f', 1);
            }

            return QStringLiteral("%1 B").arg(static_cast<qulonglong>(size));
        }

        [[nodiscard]] QString attachmentItemText(
            const javelin::jmap::submission::DraftAttachment& attachment)
        {
            const auto displayName =
                !attachment.displayName.empty()
                    ? QString::fromStdString(attachment.displayName)
                    : QFileInfo{QString::fromStdString(attachment.localFilePath)}.fileName();
            const auto mediaType =
                attachment.mediaType.empty() ? QStringLiteral("attachment")
                                             : QString::fromStdString(attachment.mediaType);
            return QStringLiteral("%1  •  %2  •  %3")
                .arg(displayName, mediaType, attachmentSizeLabel(attachment.size));
        }

        [[nodiscard]] std::string plainTextFromHtml(const QString& html)
        {
            QTextDocument document;
            document.setHtml(html);
            return document.toPlainText().toStdString();
        }

        [[nodiscard]] QString identityDisplayText(
            const javelin::jmap::domain::Identity& identity)
        {
            if (!identity.name.empty())
            {
                return QStringLiteral("%1 <%2>")
                    .arg(QString::fromStdString(identity.name),
                         QString::fromStdString(identity.email));
            }

            return QString::fromStdString(identity.email);
        }

        [[nodiscard]] std::optional<javelin::jmap::LiveConnectionSettings>
        liveSettings(const std::string_view accountId, QString* errorMessage = nullptr)
        {
            const auto settings =
                javelin::gui::settings::PreferencesDialog::loadSettingsForAccount(
                    QString::fromStdString(std::string{accountId}));
            if (settings.sessionUrl.isEmpty() || settings.loginEmail.isEmpty() ||
                settings.apiKey.isEmpty())
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = QStringLiteral(
                        "Set Session URL, Login Email, and API Key in Preferences first.");
                }
                return std::nullopt;
            }

            return javelin::jmap::LiveConnectionSettings{
                .sessionUrl = settings.sessionUrl.toStdString(),
                .loginEmail = settings.loginEmail.toStdString(),
                .apiKey = settings.apiKey.toStdString(),
            };
        }

    } // namespace

    ComposeTabWidget::ComposeTabWidget(
        javelin::jmap::submission::ComposeService& composeService,
        javelin::jmap::cache::IdentityRepository& identityRepository,
        javelin::jmap::submission::DraftSnapshot snapshot, QWidget* parent)
        : QWidget(parent), m_composeService(composeService),
          m_identityRepository(identityRepository), m_snapshot(std::move(snapshot))
    {
        setupUi();
        createToolbarActions();
        loadIdentities();
        applySnapshotToUi();

        m_autosaveTimer = new QTimer(this);
        m_autosaveTimer->setSingleShot(true);
        m_autosaveTimer->setInterval(350);
        connect(m_autosaveTimer, &QTimer::timeout, this, &ComposeTabWidget::persistWorkingCopy);

        connect(m_fromCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this](int)
                {
                    if (m_syncingUi)
                    {
                        return;
                    }

                    syncSnapshotFromUi();
                    scheduleWorkingCopySave();
                });
        for (auto* edit : {m_toEdit, m_ccEdit, m_bccEdit, m_subjectEdit})
        {
            connect(edit, &QLineEdit::textChanged, this,
                    [this](const QString&)
                    {
                        if (m_syncingUi)
                        {
                            return;
                        }

                        syncSnapshotFromUi();
                        scheduleWorkingCopySave();
                    });
        }

        connect(m_richTextEdit, &QTextEdit::textChanged, this,
                [this]
                {
                    if (m_syncingUi || m_snapshot.editorMode ==
                                            javelin::jmap::submission::BodyEditorMode::RawHtml)
                    {
                        return;
                    }

                    syncSnapshotFromUi();
                    refreshPreview();
                    scheduleWorkingCopySave();
                });
        connect(m_htmlSourceEdit, &QPlainTextEdit::textChanged, this,
                [this]
                {
                    if (m_syncingUi || m_snapshot.editorMode !=
                                            javelin::jmap::submission::BodyEditorMode::RawHtml)
                    {
                        return;
                    }

                    syncSnapshotFromUi();
                    refreshPreview();
                    scheduleWorkingCopySave();
                });
        connect(m_editorTabs, &QTabWidget::currentChanged, this,
                [this](const int index)
                {
                    if (m_syncingUi)
                    {
                        return;
                    }

                    if (index == htmlSourceTabIndex &&
                        m_snapshot.editorMode !=
                            javelin::jmap::submission::BodyEditorMode::RawHtml)
                    {
                        syncHtmlSourceFromRichText();
                        m_snapshot.editorMode =
                            javelin::jmap::submission::BodyEditorMode::RawHtml;
                    }
                    else if (index == richEditorTabIndex &&
                             m_snapshot.editorMode ==
                                 javelin::jmap::submission::BodyEditorMode::RawHtml)
                    {
                        syncRichTextFromHtmlSource();
                        m_snapshot.editorMode =
                            javelin::jmap::submission::BodyEditorMode::RichText;
                    }

                    updateEditorModeUi();
                    syncSnapshotFromUi();
                    refreshPreview();
                    scheduleWorkingCopySave();
                });

        connect(m_addAttachmentButton, &QPushButton::clicked, this,
                &ComposeTabWidget::addAttachments);
        connect(m_removeAttachmentButton, &QPushButton::clicked, this,
                &ComposeTabWidget::removeSelectedAttachment);
        connect(m_saveDraftButton, &QPushButton::clicked, this,
                [this] { startSaveDraft(false); });
        connect(m_sendButton, &QPushButton::clicked, this, &ComposeTabWidget::startSend);
        connect(m_closeButton, &QPushButton::clicked, this, &ComposeTabWidget::requestClose);

        refreshPreview();
        updateEditorModeUi();
        updateTabTitle();
    }

    QString ComposeTabWidget::tabTitle() const
    {
        const auto subject = m_subjectEdit->text().trimmed();
        return subject.isEmpty() ? defaultTitleForMode(m_snapshot.mode) : subject;
    }

    std::string ComposeTabWidget::accountId() const
    {
        return m_snapshot.accountId;
    }

    std::string ComposeTabWidget::composeSessionId() const
    {
        return m_snapshot.composeSessionId;
    }

    std::optional<std::string> ComposeTabWidget::draftEmailId() const
    {
        return m_snapshot.draftEmailId;
    }

    javelin::jmap::submission::DraftSnapshot ComposeTabWidget::snapshot() const
    {
        return m_snapshot;
    }

    bool ComposeTabWidget::isEmptyDraft() const
    {
        const auto subject = m_subjectEdit->text().trimmed();
        const auto richPlain = m_richTextEdit->toPlainText().trimmed();
        const auto rawHtml = m_htmlSourceEdit->toPlainText().trimmed();
        return subject.isEmpty() && m_toEdit->text().trimmed().isEmpty() &&
               m_ccEdit->text().trimmed().isEmpty() && m_bccEdit->text().trimmed().isEmpty() &&
               richPlain.isEmpty() && rawHtml.isEmpty() && m_snapshot.attachments.empty();
    }

    bool ComposeTabWidget::closeWithoutPrompt() const
    {
        return m_closeWithoutPrompt;
    }

    bool ComposeTabWidget::operationInFlight() const
    {
        return m_operationInFlight;
    }

    void ComposeTabWidget::saveDraftAndClose()
    {
        startSaveDraft(true);
    }

    void ComposeTabWidget::setupUi()
    {
        setObjectName(QStringLiteral("composeTab"));
        setStyleSheet(QStringLiteral(
            "#composeTab { background: #1d2026; }"
            "#composeHeader { background: #232833; border: 1px solid #333c4b; border-radius: 14px; }"
            "QLineEdit, QComboBox, QPlainTextEdit, QTextEdit, QListWidget {"
            "  background: #161a20; color: #eef2f7; border: 1px solid #394354; border-radius: 8px;"
            "}"
            "QLineEdit, QComboBox { padding: 6px 8px; }"
            "QToolBar { background: #202632; border: 1px solid #394354; border-radius: 10px; spacing: 4px; }"
            "QPushButton { background: #2b3341; color: #eef2f7; border: 1px solid #43506a; border-radius: 9px; padding: 7px 12px; }"
            "QPushButton:hover { background: #334055; }"
            "QPushButton:disabled { color: #8a93a5; background: #232833; }"
            "QTabWidget::pane { border: 1px solid #333c4b; border-radius: 12px; background: #20242d; }"
            "QTabBar::tab { background: #262c38; color: #ced7e4; padding: 8px 14px; border-top-left-radius: 8px; border-top-right-radius: 8px; }"
            "QTabBar::tab:selected { background: #38455e; color: #ffffff; }"));

        auto* rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(16, 16, 16, 16);
        rootLayout->setSpacing(12);

        auto* headerFrame = new QFrame(this);
        headerFrame->setObjectName(QStringLiteral("composeHeader"));
        auto* headerLayout = new QVBoxLayout(headerFrame);
        headerLayout->setContentsMargins(16, 16, 16, 16);
        headerLayout->setSpacing(10);

        auto* headingRow = new QHBoxLayout();
        auto* titleLabel = new QLabel(QStringLiteral("Compose"), headerFrame);
        auto titleFont = titleLabel->font();
        titleFont.setPointSize(titleFont.pointSize() + 6);
        titleFont.setBold(true);
        titleLabel->setFont(titleFont);
        m_modeHintLabel = new QLabel(headerFrame);
        m_modeHintLabel->setStyleSheet(QStringLiteral("color: #9fb2cf;"));
        headingRow->addWidget(titleLabel);
        headingRow->addStretch(1);
        headingRow->addWidget(m_modeHintLabel);
        headerLayout->addLayout(headingRow);

        auto* fromRow = new QHBoxLayout();
        auto* fromLabel = new QLabel(QStringLiteral("From"), headerFrame);
        fromLabel->setMinimumWidth(52);
        m_fromCombo = new QComboBox(headerFrame);
        fromRow->addWidget(fromLabel);
        fromRow->addWidget(m_fromCombo, 1);
        headerLayout->addLayout(fromRow);

        auto* toRow = new QHBoxLayout();
        auto* toLabel = new QLabel(QStringLiteral("To"), headerFrame);
        toLabel->setMinimumWidth(52);
        m_toEdit = new QLineEdit(headerFrame);
        m_toEdit->setPlaceholderText(QStringLiteral("alice@example.com, Bob <bob@example.com>"));
        toRow->addWidget(toLabel);
        toRow->addWidget(m_toEdit, 1);
        headerLayout->addLayout(toRow);

        auto* ccRow = new QHBoxLayout();
        auto* ccLabel = new QLabel(QStringLiteral("Cc"), headerFrame);
        ccLabel->setMinimumWidth(52);
        m_ccEdit = new QLineEdit(headerFrame);
        m_ccEdit->setPlaceholderText(QStringLiteral("Optional"));
        ccRow->addWidget(ccLabel);
        ccRow->addWidget(m_ccEdit, 1);
        headerLayout->addLayout(ccRow);

        auto* bccRow = new QHBoxLayout();
        auto* bccLabel = new QLabel(QStringLiteral("Bcc"), headerFrame);
        bccLabel->setMinimumWidth(52);
        m_bccEdit = new QLineEdit(headerFrame);
        m_bccEdit->setPlaceholderText(QStringLiteral("Optional"));
        bccRow->addWidget(bccLabel);
        bccRow->addWidget(m_bccEdit, 1);
        headerLayout->addLayout(bccRow);

        auto* subjectRow = new QHBoxLayout();
        auto* subjectLabel = new QLabel(QStringLiteral("Subject"), headerFrame);
        subjectLabel->setMinimumWidth(52);
        m_subjectEdit = new QLineEdit(headerFrame);
        m_subjectEdit->setPlaceholderText(QStringLiteral("Add a subject"));
        subjectRow->addWidget(subjectLabel);
        subjectRow->addWidget(m_subjectEdit, 1);
        headerLayout->addLayout(subjectRow);

        rootLayout->addWidget(headerFrame);

        m_formatToolbar = new QToolBar(this);
        m_formatToolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        rootLayout->addWidget(m_formatToolbar);

        m_editorTabs = new QTabWidget(this);
        m_richTextEdit = new QTextEdit(m_editorTabs);
        m_richTextEdit->setAcceptRichText(true);
        m_richTextEdit->document()->setDocumentMargin(14);
        m_htmlSourceEdit = new QPlainTextEdit(m_editorTabs);
        m_htmlSourceEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
        m_previewView = new javelin::gui::messageview::HtmlMessageView(m_editorTabs);
        m_previewView->setRemoteContentEnabled(false);
        m_editorTabs->addTab(m_richTextEdit, QStringLiteral("Compose"));
        m_editorTabs->addTab(m_htmlSourceEdit, QStringLiteral("HTML"));
        m_editorTabs->addTab(m_previewView, QStringLiteral("Preview"));
        rootLayout->addWidget(m_editorTabs, 1);

        auto* attachmentFrame = new QFrame(this);
        attachmentFrame->setObjectName(QStringLiteral("composeHeader"));
        auto* attachmentLayout = new QVBoxLayout(attachmentFrame);
        attachmentLayout->setContentsMargins(14, 14, 14, 14);
        attachmentLayout->setSpacing(10);
        auto* attachmentHeaderRow = new QHBoxLayout();
        auto* attachmentTitle = new QLabel(QStringLiteral("Attachments"), attachmentFrame);
        auto attachmentTitleFont = attachmentTitle->font();
        attachmentTitleFont.setBold(true);
        attachmentTitle->setFont(attachmentTitleFont);
        m_attachmentMetaLabel = new QLabel(QStringLiteral("No attachments yet"), attachmentFrame);
        m_attachmentMetaLabel->setStyleSheet(QStringLiteral("color: #9fb2cf;"));
        attachmentHeaderRow->addWidget(attachmentTitle);
        attachmentHeaderRow->addStretch(1);
        attachmentHeaderRow->addWidget(m_attachmentMetaLabel);
        attachmentLayout->addLayout(attachmentHeaderRow);

        m_attachmentList = new QListWidget(attachmentFrame);
        attachmentLayout->addWidget(m_attachmentList);

        auto* attachmentButtons = new QHBoxLayout();
        m_addAttachmentButton = new QPushButton(QStringLiteral("Attach Files"), attachmentFrame);
        m_removeAttachmentButton =
            new QPushButton(QStringLiteral("Remove Selected"), attachmentFrame);
        attachmentButtons->addWidget(m_addAttachmentButton);
        attachmentButtons->addWidget(m_removeAttachmentButton);
        attachmentButtons->addStretch(1);
        attachmentLayout->addLayout(attachmentButtons);

        rootLayout->addWidget(attachmentFrame);

        auto* footerRow = new QHBoxLayout();
        auto* helperLabel = new QLabel(
            QStringLiteral("Rich text for everyday composing, raw HTML when you need exact control."),
            this);
        helperLabel->setStyleSheet(QStringLiteral("color: #95a6c2;"));
        m_saveDraftButton = new QPushButton(QStringLiteral("Save Draft"), this);
        m_sendButton = new QPushButton(QStringLiteral("Send"), this);
        m_closeButton = new QPushButton(QStringLiteral("Close"), this);
        footerRow->addWidget(helperLabel, 1);
        footerRow->addWidget(m_saveDraftButton);
        footerRow->addWidget(m_sendButton);
        footerRow->addWidget(m_closeButton);
        rootLayout->addLayout(footerRow);
    }

    void ComposeTabWidget::createToolbarActions()
    {
        m_boldAction =
            m_formatToolbar->addAction(QIcon::fromTheme(QStringLiteral("format-text-bold")),
                                       QStringLiteral("Bold"));
        m_boldAction->setCheckable(true);
        connect(m_boldAction, &QAction::triggered, this, &ComposeTabWidget::toggleBold);

        m_italicAction =
            m_formatToolbar->addAction(QIcon::fromTheme(QStringLiteral("format-text-italic")),
                                       QStringLiteral("Italic"));
        m_italicAction->setCheckable(true);
        connect(m_italicAction, &QAction::triggered, this, &ComposeTabWidget::toggleItalic);

        m_underlineAction =
            m_formatToolbar->addAction(QIcon::fromTheme(QStringLiteral("format-text-underline")),
                                       QStringLiteral("Underline"));
        m_underlineAction->setCheckable(true);
        connect(m_underlineAction, &QAction::triggered, this, &ComposeTabWidget::toggleUnderline);

        m_strikethroughAction =
            m_formatToolbar->addAction(
                QIcon::fromTheme(QStringLiteral("format-text-strikethrough")),
                QStringLiteral("Strike"));
        m_strikethroughAction->setCheckable(true);
        connect(m_strikethroughAction, &QAction::triggered, this,
                &ComposeTabWidget::toggleStrikethrough);

        m_formatToolbar->addSeparator();

        auto* bulletAction =
            m_formatToolbar->addAction(QIcon::fromTheme(QStringLiteral("format-list-unordered")),
                                       QStringLiteral("Bullets"));
        connect(bulletAction, &QAction::triggered, this, &ComposeTabWidget::insertBulletList);

        auto* numberedAction =
            m_formatToolbar->addAction(QIcon::fromTheme(QStringLiteral("format-list-ordered")),
                                       QStringLiteral("Numbering"));
        connect(numberedAction, &QAction::triggered, this, &ComposeTabWidget::insertNumberedList);

        auto* linkAction =
            m_formatToolbar->addAction(QIcon::fromTheme(QStringLiteral("insert-link")),
                                       QStringLiteral("Link"));
        connect(linkAction, &QAction::triggered, this, &ComposeTabWidget::insertLink);

        m_formatToolbar->addSeparator();

        auto* alignLeftAction =
            m_formatToolbar->addAction(QIcon::fromTheme(QStringLiteral("format-justify-left")),
                                       QStringLiteral("Left"));
        connect(alignLeftAction, &QAction::triggered, this, &ComposeTabWidget::alignLeft);

        auto* alignCenterAction =
            m_formatToolbar->addAction(QIcon::fromTheme(QStringLiteral("format-justify-center")),
                                       QStringLiteral("Center"));
        connect(alignCenterAction, &QAction::triggered, this, &ComposeTabWidget::alignCenter);

        auto* alignRightAction =
            m_formatToolbar->addAction(QIcon::fromTheme(QStringLiteral("format-justify-right")),
                                       QStringLiteral("Right"));
        connect(alignRightAction, &QAction::triggered, this, &ComposeTabWidget::alignRight);

        auto* clearAction =
            m_formatToolbar->addAction(QIcon::fromTheme(QStringLiteral("edit-clear-format")),
                                       QStringLiteral("Clear"));
        connect(clearAction, &QAction::triggered, this, &ComposeTabWidget::clearFormatting);
    }

    void ComposeTabWidget::loadIdentities()
    {
        const auto identitiesResult = m_identityRepository.listByAccount(m_snapshot.accountId);
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&identitiesResult))
        {
            Q_EMIT statusMessageRequested(error->message, 10000);
            return;
        }

        m_identities = std::get<std::vector<javelin::jmap::domain::Identity>>(identitiesResult);
        m_fromCombo->clear();
        for (const auto& identity : m_identities)
        {
            m_fromCombo->addItem(identityDisplayText(identity),
                                 QString::fromStdString(identity.id));
        }
    }

    void ComposeTabWidget::applySnapshotToUi()
    {
        m_syncingUi = true;
        const QSignalBlocker fromBlocker{m_fromCombo};
        const QSignalBlocker toBlocker{m_toEdit};
        const QSignalBlocker ccBlocker{m_ccEdit};
        const QSignalBlocker bccBlocker{m_bccEdit};
        const QSignalBlocker subjectBlocker{m_subjectEdit};
        const QSignalBlocker richBlocker{m_richTextEdit};
        const QSignalBlocker htmlBlocker{m_htmlSourceEdit};
        const QSignalBlocker tabBlocker{m_editorTabs};

        const auto identityIndex =
            m_fromCombo->findData(QString::fromStdString(m_snapshot.identityId));
        if (identityIndex >= 0)
        {
            m_fromCombo->setCurrentIndex(identityIndex);
        }

        m_toEdit->setText(formatAddresses(m_snapshot.to));
        m_ccEdit->setText(formatAddresses(m_snapshot.cc));
        m_bccEdit->setText(formatAddresses(m_snapshot.bcc));
        m_subjectEdit->setText(
            m_snapshot.subject.has_value() ? QString::fromStdString(*m_snapshot.subject)
                                           : QString{});
        m_richTextEdit->setHtml(QString::fromStdString(m_snapshot.htmlBody));
        m_htmlSourceEdit->setPlainText(QString::fromStdString(m_snapshot.htmlBody));
        m_editorTabs->setCurrentIndex(
            m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::RawHtml
                ? htmlSourceTabIndex
                : richEditorTabIndex);
        populateAttachments();
        m_syncingUi = false;
    }

    void ComposeTabWidget::populateAttachments()
    {
        m_attachmentList->clear();
        for (const auto& attachment : m_snapshot.attachments)
        {
            m_attachmentList->addItem(attachmentItemText(attachment));
        }

        m_attachmentMetaLabel->setText(
            m_snapshot.attachments.empty()
                ? QStringLiteral("No attachments yet")
                : QStringLiteral("%1 file%2")
                      .arg(static_cast<qulonglong>(m_snapshot.attachments.size()))
                      .arg(m_snapshot.attachments.size() == 1 ? QString{} : QStringLiteral("s")));
        m_removeAttachmentButton->setEnabled(!m_snapshot.attachments.empty());
    }

    void ComposeTabWidget::refreshPreview()
    {
        const auto html =
            m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::RawHtml
                ? m_htmlSourceEdit->toPlainText()
                : m_richTextEdit->document()->toHtml();
        m_previewView->setDocumentHtml(html.toStdString());
    }

    void ComposeTabWidget::syncSnapshotFromUi()
    {
        m_snapshot.identityId = m_fromCombo->currentData().toString().toStdString();
        m_snapshot.to = parseAddresses(m_toEdit->text());
        m_snapshot.cc = parseAddresses(m_ccEdit->text());
        m_snapshot.bcc = parseAddresses(m_bccEdit->text());
        m_snapshot.subject = m_subjectEdit->text().trimmed().isEmpty()
                                 ? std::nullopt
                                 : std::optional<std::string>{
                                       m_subjectEdit->text().trimmed().toStdString()};

        if (m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::RawHtml)
        {
            const auto html = m_htmlSourceEdit->toPlainText();
            m_snapshot.htmlBody = html.toStdString();
            m_snapshot.plainTextBody = plainTextFromHtml(html);
        }
        else
        {
            m_snapshot.htmlBody = m_richTextEdit->document()->toHtml().toStdString();
            m_snapshot.plainTextBody = m_richTextEdit->toPlainText().toStdString();
        }

        updateTabTitle();
    }

    void ComposeTabWidget::syncRichTextFromHtmlSource()
    {
        m_syncingUi = true;
        const QSignalBlocker richBlocker{m_richTextEdit};
        m_richTextEdit->setHtml(m_htmlSourceEdit->toPlainText());
        m_syncingUi = false;
    }

    void ComposeTabWidget::syncHtmlSourceFromRichText()
    {
        m_syncingUi = true;
        const QSignalBlocker htmlBlocker{m_htmlSourceEdit};
        m_htmlSourceEdit->setPlainText(m_richTextEdit->document()->toHtml());
        m_syncingUi = false;
    }

    void ComposeTabWidget::scheduleWorkingCopySave()
    {
        if (m_operationInFlight)
        {
            return;
        }

        m_autosaveTimer->start();
    }

    void ComposeTabWidget::persistWorkingCopy()
    {
        syncSnapshotFromUi();
        if (const auto error = m_composeService.storeWorkingCopy(m_snapshot))
        {
            Q_EMIT statusMessageRequested(error->message, 10000);
            return;
        }

        Q_EMIT statusMessageRequested(QStringLiteral("Saved working copy locally."), 2500);
    }

    void ComposeTabWidget::setBusy(const bool busy)
    {
        m_operationInFlight = busy;
        m_fromCombo->setEnabled(!busy);
        m_toEdit->setEnabled(!busy);
        m_ccEdit->setEnabled(!busy);
        m_bccEdit->setEnabled(!busy);
        m_subjectEdit->setEnabled(!busy);
        m_richTextEdit->setEnabled(!busy);
        m_htmlSourceEdit->setEnabled(!busy);
        m_editorTabs->setEnabled(!busy);
        m_formatToolbar->setEnabled(!busy && m_editorTabs->currentIndex() == richEditorTabIndex);
        m_addAttachmentButton->setEnabled(!busy);
        m_removeAttachmentButton->setEnabled(!busy && !m_snapshot.attachments.empty());
        m_saveDraftButton->setEnabled(!busy);
        m_sendButton->setEnabled(!busy);
        m_closeButton->setEnabled(!busy);
    }

    void ComposeTabWidget::updateEditorModeUi()
    {
        const bool richMode =
            m_editorTabs->currentIndex() == richEditorTabIndex &&
            m_snapshot.editorMode != javelin::jmap::submission::BodyEditorMode::RawHtml;
        m_formatToolbar->setEnabled(!m_operationInFlight && richMode);
        m_modeHintLabel->setText(
            m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::RawHtml
                ? QStringLiteral("Editing raw HTML")
                : QStringLiteral("Editing rich text"));
    }

    void ComposeTabWidget::updateTabTitle()
    {
        Q_EMIT titleChanged(tabTitle());
    }

    void ComposeTabWidget::addAttachments()
    {
        const auto filePaths =
            QFileDialog::getOpenFileNames(this, QStringLiteral("Attach Files"));
        if (filePaths.empty())
        {
            return;
        }

        for (const auto& filePath : filePaths)
        {
            const QFileInfo info{filePath};
            m_snapshot.attachments.push_back(javelin::jmap::submission::DraftAttachment{
                .localFilePath = filePath.toStdString(),
                .displayName = info.fileName().toStdString(),
                .mediaType = {},
                .size = static_cast<std::uint64_t>(info.size()),
                .blobId = std::nullopt,
                .inlineDisposition = false,
                .contentId = std::nullopt,
            });
        }

        populateAttachments();
        scheduleWorkingCopySave();
    }

    void ComposeTabWidget::removeSelectedAttachment()
    {
        const auto row = m_attachmentList->currentRow();
        if (row < 0 || static_cast<std::size_t>(row) >= m_snapshot.attachments.size())
        {
            return;
        }

        m_snapshot.attachments.erase(m_snapshot.attachments.begin() + row);
        populateAttachments();
        scheduleWorkingCopySave();
    }

    void ComposeTabWidget::requestClose()
    {
        Q_EMIT closeRequested();
    }

    void ComposeTabWidget::startSaveDraft(const bool closeAfterSave)
    {
        if (m_operationInFlight)
        {
            return;
        }

        QString errorMessage;
        const auto settings = liveSettings(m_snapshot.accountId, &errorMessage);
        if (!settings.has_value())
        {
            Q_EMIT statusMessageRequested(errorMessage, 10000);
            return;
        }

        syncSnapshotFromUi();
        if (m_autosaveTimer->isActive())
        {
            m_autosaveTimer->stop();
        }
        if (const auto error = m_composeService.storeWorkingCopy(m_snapshot))
        {
            Q_EMIT statusMessageRequested(error->message, 10000);
            return;
        }

        setBusy(true);
        m_closeAfterSave = closeAfterSave;
        Q_EMIT statusMessageRequested(QStringLiteral("Saving draft..."), 5000);
        auto task = m_composeService.saveDraft(*settings, m_snapshot);
        QCoro::connect(
            std::move(task), this,
            [this](std::variant<javelin::jmap::submission::DraftSaveSummary,
                                javelin::jmap::LiveRefreshError> result)
            {
                setBusy(false);
                if (const auto* error =
                        std::get_if<javelin::jmap::LiveRefreshError>(&result))
                {
                    Q_EMIT statusMessageRequested(error->message, 10000);
                    m_closeAfterSave = false;
                    return;
                }

                const auto& summary =
                    std::get<javelin::jmap::submission::DraftSaveSummary>(result);
                m_snapshot.draftEmailId = summary.draftEmailId;
                if (const auto error = m_composeService.storeWorkingCopy(m_snapshot))
                {
                    Q_EMIT statusMessageRequested(error->message, 10000);
                    m_closeAfterSave = false;
                    return;
                }

                Q_EMIT statusMessageRequested(QStringLiteral("Draft saved."), 5000);
                if (m_closeAfterSave)
                {
                    m_closeAfterSave = false;
                    if (const auto error =
                            m_composeService.discard(m_snapshot.composeSessionId))
                    {
                        Q_EMIT statusMessageRequested(error->message, 10000);
                        return;
                    }
                    m_closeWithoutPrompt = true;
                    Q_EMIT closeRequested();
                }
            });
    }

    void ComposeTabWidget::startSend()
    {
        if (m_operationInFlight)
        {
            return;
        }

        QString errorMessage;
        const auto settings = liveSettings(m_snapshot.accountId, &errorMessage);
        if (!settings.has_value())
        {
            Q_EMIT statusMessageRequested(errorMessage, 10000);
            return;
        }

        syncSnapshotFromUi();
        if (m_snapshot.to.empty())
        {
            Q_EMIT statusMessageRequested(QStringLiteral("Add at least one recipient before sending."),
                                          7000);
            return;
        }

        setBusy(true);
        Q_EMIT statusMessageRequested(QStringLiteral("Sending message..."), 5000);
        auto task = m_composeService.send(*settings, m_snapshot);
        QCoro::connect(
            std::move(task), this,
            [this](std::variant<javelin::jmap::submission::SendSummary,
                                javelin::jmap::LiveRefreshError> result)
            {
                setBusy(false);
                if (const auto* error =
                        std::get_if<javelin::jmap::LiveRefreshError>(&result))
                {
                    Q_EMIT statusMessageRequested(error->message, 10000);
                    return;
                }

                const auto& summary =
                    std::get<javelin::jmap::submission::SendSummary>(result);
                m_snapshot.draftEmailId = summary.draftEmailId;
                m_closeWithoutPrompt = true;
                Q_EMIT statusMessageRequested(QStringLiteral("Message sent."), 7000);
                Q_EMIT closeRequested();
            });
    }

    void ComposeTabWidget::toggleBold()
    {
        QTextCharFormat format;
        format.setFontWeight(m_boldAction->isChecked() ? QFont::Bold : QFont::Normal);
        m_richTextEdit->mergeCurrentCharFormat(format);
    }

    void ComposeTabWidget::toggleItalic()
    {
        QTextCharFormat format;
        format.setFontItalic(m_italicAction->isChecked());
        m_richTextEdit->mergeCurrentCharFormat(format);
    }

    void ComposeTabWidget::toggleUnderline()
    {
        QTextCharFormat format;
        format.setFontUnderline(m_underlineAction->isChecked());
        m_richTextEdit->mergeCurrentCharFormat(format);
    }

    void ComposeTabWidget::toggleStrikethrough()
    {
        QTextCharFormat format;
        format.setFontStrikeOut(m_strikethroughAction->isChecked());
        m_richTextEdit->mergeCurrentCharFormat(format);
    }

    void ComposeTabWidget::insertBulletList()
    {
        QTextListFormat listFormat;
        listFormat.setStyle(QTextListFormat::ListDisc);
        m_richTextEdit->textCursor().createList(listFormat);
    }

    void ComposeTabWidget::insertNumberedList()
    {
        QTextListFormat listFormat;
        listFormat.setStyle(QTextListFormat::ListDecimal);
        m_richTextEdit->textCursor().createList(listFormat);
    }

    void ComposeTabWidget::alignLeft()
    {
        m_richTextEdit->setAlignment(Qt::AlignLeft);
    }

    void ComposeTabWidget::alignCenter()
    {
        m_richTextEdit->setAlignment(Qt::AlignHCenter);
    }

    void ComposeTabWidget::alignRight()
    {
        m_richTextEdit->setAlignment(Qt::AlignRight);
    }

    void ComposeTabWidget::clearFormatting()
    {
        QTextCursor cursor = m_richTextEdit->textCursor();
        QTextCharFormat charFormat;
        charFormat.setFontWeight(QFont::Normal);
        charFormat.setFontItalic(false);
        charFormat.setFontUnderline(false);
        charFormat.setFontStrikeOut(false);
        cursor.mergeCharFormat(charFormat);
        QTextBlockFormat blockFormat;
        blockFormat.setAlignment(Qt::AlignLeft);
        cursor.mergeBlockFormat(blockFormat);
        m_richTextEdit->setTextCursor(cursor);
        m_boldAction->setChecked(false);
        m_italicAction->setChecked(false);
        m_underlineAction->setChecked(false);
        m_strikethroughAction->setChecked(false);
    }

    void ComposeTabWidget::insertLink()
    {
        bool accepted = false;
        const auto existingSelection = m_richTextEdit->textCursor().selectedText();
        const auto url = QInputDialog::getText(this, QStringLiteral("Insert Link"),
                                               QStringLiteral("URL"),
                                               QLineEdit::Normal,
                                               QStringLiteral("https://"), &accepted);
        if (!accepted || url.trimmed().isEmpty())
        {
            return;
        }

        QTextCursor cursor = m_richTextEdit->textCursor();
        const auto label =
            existingSelection.isEmpty() ? url.trimmed() : existingSelection;
        cursor.insertHtml(QStringLiteral("<a href=\"%1\">%2</a>")
                              .arg(url.toHtmlEscaped(), label.toHtmlEscaped()));
    }

} // namespace javelin::gui::compose
