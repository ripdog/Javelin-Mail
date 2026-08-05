#include "gui/compose/ComposeTabWidget.h"

#include "app/ComposeApplicationPorts.h"
#include "gui/compose/ComposeBodyConverter.h"
#include "gui/compose/ComposerInlineImageCodec.h"
#include "gui/compose/JavelinComposerEdit.h"
#include "gui/messageview/HtmlMessageView.h"
#include "gui/settings/GuiSettings.h"
#include "gui/widgets/EmailAddressLineEdit.h"
#include "jmap/cache/IdentityReader.h"
#include "jmap/contacts/ContactIdentityLookup.h"

#include <QCoroTask>

#include <KActionCollection>
#include <KLocalizedString>
#include <KPIMTextEdit/RichTextComposerControler>
#include <KPIMTextEdit/RichTextComposerImages>
#include <MessageComposer/TextPart>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
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
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <unordered_map>

namespace javelin::gui::compose
{

    namespace
    {

        constexpr auto richEditorTabIndex = 0;
        constexpr auto previewTabIndex = 1;
        constexpr auto htmlFormatIndex = 0;
        constexpr auto plainTextFormatIndex = 1;
        constexpr auto senderIdentityIdRole = Qt::UserRole;
        constexpr auto senderAccountIdRole = Qt::UserRole + 1;

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

        [[nodiscard]] QString displayAddress(const javelin::jmap::domain::EmailAddress& address)
        {
            if (address.name.has_value() && !address.name->empty())
            {
                return QStringLiteral("%1 <%2>").arg(QString::fromStdString(*address.name),
                                                     QString::fromStdString(address.email));
            }

            return QString::fromStdString(address.email);
        }

        [[nodiscard]] QString
        formatAddresses(const std::vector<javelin::jmap::domain::EmailAddress>& addresses)
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
                const auto name = trimmed.left(openBracket).trimmed().remove(QLatin1Char('"'));
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
                return i18nc("@item file size", "%1 GB",
                             QString::number(static_cast<double>(size) / gib, 'f', 1));
            }
            if (size >= static_cast<std::uint64_t>(mib))
            {
                return i18nc("@item file size", "%1 MB",
                             QString::number(static_cast<double>(size) / mib, 'f', 1));
            }
            if (size >= static_cast<std::uint64_t>(kib))
            {
                return i18nc("@item file size", "%1 KB",
                             QString::number(static_cast<double>(size) / kib, 'f', 1));
            }

            return i18nc("@item file size", "%1 B", static_cast<qulonglong>(size));
        }

        [[nodiscard]] QString
        attachmentItemText(const javelin::jmap::submission::DraftAttachment& attachment)
        {
            const auto displayName =
                !attachment.displayName.empty()
                    ? QString::fromStdString(attachment.displayName)
                    : QFileInfo{QString::fromStdString(attachment.localFilePath)}.fileName();
            const auto mediaType = attachment.mediaType.empty()
                                       ? i18nc("@item unknown attachment media type", "attachment")
                                       : QString::fromStdString(attachment.mediaType);
            return QStringLiteral("%1  •  %2  •  %3")
                .arg(displayName, mediaType, attachmentSizeLabel(attachment.size));
        }

        [[nodiscard]] QString
        attachmentDisplayName(const javelin::jmap::submission::DraftAttachment& attachment)
        {
            if (!attachment.displayName.empty())
            {
                return QString::fromStdString(attachment.displayName);
            }
            if (!attachment.localFilePath.empty())
            {
                return QFileInfo{QString::fromStdString(attachment.localFilePath)}.fileName();
            }
            return i18n("Attachment");
        }

        [[nodiscard]] QIcon
        attachmentIcon(const javelin::jmap::submission::DraftAttachment& attachment)
        {
            QFileIconProvider iconProvider;
            auto icon = iconProvider.icon(QFileInfo{attachmentDisplayName(attachment)});
            if (!icon.isNull())
            {
                return icon;
            }

            const QMimeDatabase mimeDatabase;
            const auto mimeType =
                mimeDatabase.mimeTypeForName(QString::fromStdString(attachment.mediaType));
            icon = QIcon::fromTheme(mimeType.iconName());
            if (icon.isNull())
            {
                icon = QIcon::fromTheme(mimeType.genericIconName());
            }
            if (icon.isNull())
            {
                icon = QIcon::fromTheme(QStringLiteral("mail-attachment"));
            }
            if (icon.isNull())
            {
                icon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
            }
            return icon;
        }

        [[nodiscard]] bool
        isImageAttachment(const javelin::jmap::submission::DraftAttachment& attachment)
        {
            if (!attachment.mediaType.empty())
            {
                return attachment.mediaType.rfind("image/", 0) == 0;
            }
            if (attachment.localFilePath.empty())
            {
                return false;
            }

            QMimeDatabase mimeDatabase;
            const auto mimeType = mimeDatabase.mimeTypeForFile(
                QString::fromStdString(attachment.localFilePath), QMimeDatabase::MatchContent);
            return mimeType.name().startsWith(QStringLiteral("image/"));
        }

        [[nodiscard]] QString detectedMediaType(const QString& filePath)
        {
            QMimeDatabase mimeDatabase;
            const auto mimeType =
                mimeDatabase.mimeTypeForFile(filePath, QMimeDatabase::MatchContent);
            return mimeType.isValid() ? mimeType.name()
                                      : QStringLiteral("application/octet-stream");
        }

        [[nodiscard]] std::string newContentId()
        {
            const auto uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            return QStringLiteral("javelin-%1@inline").arg(uuid).toStdString();
        }

        [[nodiscard]] QString draftAssetDirectory(const std::string& composeSessionId)
        {
            return QDir{QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)}.filePath(
                QStringLiteral("draft-assets/%1").arg(QString::fromStdString(composeSessionId)));
        }

        class DraftAttachmentChip : public QWidget
        {
          public:
            DraftAttachmentChip(const javelin::jmap::submission::DraftAttachment& attachment,
                                const bool embeddingAllowed, std::function<void()> removeAction,
                                std::function<void(bool)> embedAction, QWidget* parent = nullptr)
                : QWidget(parent), m_removeAction(std::move(removeAction)),
                  m_embedAction(std::move(embedAction))
            {
                setToolTip(attachmentItemText(attachment));

                auto* layout = new QHBoxLayout(this);
                layout->setContentsMargins(4, 0, 2, 0);
                layout->setSpacing(4);

                auto* iconLabel = new QLabel(this);
                iconLabel->setPixmap(attachmentIcon(attachment).pixmap(16, 16));
                iconLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

                auto* nameLabel = new QLabel(attachmentDisplayName(attachment), this);
                nameLabel->setMinimumWidth(80);
                nameLabel->setMaximumWidth(220);
                nameLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

                auto* sizeLabel = new QLabel(attachmentSizeLabel(attachment.size), this);
                sizeLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

                if (embeddingAllowed && isImageAttachment(attachment))
                {
                    auto* attachRadio = new QRadioButton(i18nc("@option:radio", "Attach"), this);
                    auto* embedRadio = new QRadioButton(i18nc("@option:radio", "Embed"), this);
                    attachRadio->setChecked(!attachment.inlineDisposition);
                    embedRadio->setChecked(attachment.inlineDisposition);
                    attachRadio->setToolTip(i18n("Send this image as an attachment"));
                    embedRadio->setToolTip(i18n("Show this image in the message body"));
                    connect(attachRadio, &QRadioButton::toggled, this,
                            [this](const bool checked)
                            {
                                if (checked && m_embedAction)
                                {
                                    m_embedAction(false);
                                }
                            });
                    connect(embedRadio, &QRadioButton::toggled, this,
                            [this](const bool checked)
                            {
                                if (checked && m_embedAction)
                                {
                                    m_embedAction(true);
                                }
                            });
                    layout->addWidget(attachRadio);
                    layout->addWidget(embedRadio);
                }

                auto* removeButton = new QToolButton(this);
                removeButton->setText(QStringLiteral("x"));
                removeButton->setToolTip(i18n("Remove attachment"));
                removeButton->setAccessibleName(i18n("Remove attachment"));
                removeButton->setAutoRaise(true);
                removeButton->setFixedSize(22, 22);
                connect(removeButton, &QToolButton::clicked, this,
                        [this]
                        {
                            if (m_removeAction)
                            {
                                m_removeAction();
                            }
                        });

                layout->addWidget(iconLabel);
                layout->addWidget(nameLabel);
                layout->addWidget(sizeLabel);
                layout->addWidget(removeButton);
            }

          private:
            std::function<void()> m_removeAction;
            std::function<void(bool)> m_embedAction;
        };

        [[nodiscard]] QString identityDisplayText(const javelin::jmap::domain::Identity& identity,
                                                  const QString& accountDisplayName)
        {
            const auto identityText =
                !identity.name.empty()
                    ? QStringLiteral("%1 <%2>").arg(QString::fromStdString(identity.name),
                                                    QString::fromStdString(identity.email))
                    : QString::fromStdString(identity.email);
            return accountDisplayName.isEmpty()
                       ? identityText
                       : QStringLiteral("%1 — %2").arg(identityText, accountDisplayName);
        }

        [[nodiscard]] bool isWildcardSenderIdentity(const javelin::jmap::domain::Identity& identity)
        {
            return identity.email.starts_with("*@");
        }

        [[nodiscard]] std::optional<javelin::app::AccountConnectionSettings>
        liveSettings(const javelin::gui::settings::GuiSettings& guiSettings,
                     const std::string_view accountId, QString* errorMessage = nullptr)
        {
            const auto settings =
                guiSettings.accountForCachedId(QString::fromStdString(std::string{accountId}));
            if (settings.sessionUrl.isEmpty() || settings.loginEmail.isEmpty() ||
                settings.apiKey.isEmpty())
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage =
                        i18n("Set Session URL, Login Email, and API Key in Preferences first.");
                }
                return std::nullopt;
            }

            return javelin::app::AccountConnectionSettings{
                .connectionId = settings.id.toStdString(),
                .revision = settings.revision,
                .sessionUrl = settings.sessionUrl.toStdString(),
                .loginEmail = settings.loginEmail.toStdString(),
                .apiKey = settings.apiKey.toStdString(),
                .refreshToken = settings.refreshToken.toStdString(),
                .tokenEndpoint = settings.tokenEndpoint.toStdString(),
                .oauthClientId = settings.oauthClientId.toStdString(),
                .oauthIssuer = settings.oauthIssuer.toStdString(),
                .oauthResource = settings.oauthResource.toStdString(),
                .oauthScope = settings.oauthScope.toStdString(),
                .revocationEndpoint = settings.revocationEndpoint.toStdString(),
            };
        }

    } // namespace

    ComposeTabWidget::ComposeTabWidget(
        javelin::gui::settings::GuiSettings& settings,
        javelin::app::ComposeCommandPort& composeCommandPort,
        javelin::jmap::cache::IdentityReader& identityRepository,
        javelin::jmap::contacts::ContactIdentityLookup& contactIdentityLookup,
        javelin::jmap::submission::DraftSnapshot snapshot, QWidget* parent)
        : QWidget(parent), m_settings(settings), m_composeCommandPort(composeCommandPort),
          m_identityRepository(identityRepository), m_contactIdentityLookup(contactIdentityLookup),
          m_snapshot(std::move(snapshot))
    {
        setAcceptDrops(true);
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
        connect(m_bodyFormatCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                &ComposeTabWidget::switchBodyFormat);
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

        connect(m_richTextEdit, &JavelinComposerEdit::attachmentPathsRequested, this,
                &ComposeTabWidget::addAttachmentPaths);
        connect(m_richTextEdit, &JavelinComposerEdit::inlineImageRequested, this,
                &ComposeTabWidget::addPastedInlineImage);
        connect(m_richTextEdit, &QTextEdit::textChanged, this,
                [this]
                {
                    if (m_syncingUi)
                    {
                        return;
                    }

                    syncSnapshotFromUi();
                    refreshPreview();
                    scheduleWorkingCopySave();
                });
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
        return subject.isEmpty() && m_toEdit->text().trimmed().isEmpty() &&
               m_ccEdit->text().trimmed().isEmpty() && m_bccEdit->text().trimmed().isEmpty() &&
               body.trimmed().isEmpty() && m_snapshot.attachments.empty();
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
        auto* fromLabel = new QLabel(i18nc("@label email sender", "From"), headerWidget);
        fromLabel->setMinimumWidth(52);
        m_fromCombo = new QComboBox(headerWidget);
        auto* formatLabel = new QLabel(i18n("Format"), headerWidget);
        m_bodyFormatCombo = new QComboBox(headerWidget);
        m_bodyFormatCombo->addItem(i18nc("@item message body format", "HTML"));
        m_bodyFormatCombo->addItem(i18nc("@item message body format", "Plain text"));
        fromRow->addWidget(fromLabel);
        fromRow->addWidget(m_fromCombo, 1);
        fromRow->addSpacing(8);
        fromRow->addWidget(formatLabel);
        fromRow->addWidget(m_bodyFormatCombo);
        headerLayout->addLayout(fromRow);

        auto* toRow = new QHBoxLayout();
        auto* toLabel = new QLabel(i18nc("@label email recipients", "To"), headerWidget);
        toLabel->setMinimumWidth(52);
        m_toEdit = new widgets::EmailAddressLineEdit(true, headerWidget);
        m_toEdit->setPlaceholderText(QStringLiteral("alice@example.com, Bob <bob@example.com>"));
        m_ccButton = new QToolButton(headerWidget);
        m_ccButton->setText(i18nc("@action:button show carbon-copy field", "Cc"));
        m_ccButton->setCheckable(true);
        m_ccButton->setAutoRaise(true);
        m_bccButton = new QToolButton(headerWidget);
        m_bccButton->setText(i18nc("@action:button show blind-carbon-copy field", "Bcc"));
        m_bccButton->setCheckable(true);
        m_bccButton->setAutoRaise(true);
        toRow->addWidget(toLabel);
        toRow->addWidget(m_toEdit, 1);
        toRow->addWidget(m_ccButton);
        toRow->addWidget(m_bccButton);
        headerLayout->addLayout(toRow);

        m_ccRow = new QWidget(headerWidget);
        auto* ccRowLayout = new QHBoxLayout(m_ccRow);
        ccRowLayout->setContentsMargins(0, 0, 0, 0);
        auto* ccLabel = new QLabel(i18nc("@label email carbon-copy recipients", "Cc"), m_ccRow);
        ccLabel->setMinimumWidth(52);
        m_ccEdit = new widgets::EmailAddressLineEdit(true, m_ccRow);
        m_ccEdit->setPlaceholderText(i18nc("@info:placeholder", "Optional"));
        ccRowLayout->addWidget(ccLabel);
        ccRowLayout->addWidget(m_ccEdit, 1);
        headerLayout->addWidget(m_ccRow);

        m_bccRow = new QWidget(headerWidget);
        auto* bccRowLayout = new QHBoxLayout(m_bccRow);
        bccRowLayout->setContentsMargins(0, 0, 0, 0);
        auto* bccLabel =
            new QLabel(i18nc("@label email blind-carbon-copy recipients", "Bcc"), m_bccRow);
        bccLabel->setMinimumWidth(52);
        m_bccEdit = new widgets::EmailAddressLineEdit(true, m_bccRow);
        m_bccEdit->setPlaceholderText(i18nc("@info:placeholder", "Optional"));
        bccRowLayout->addWidget(bccLabel);
        bccRowLayout->addWidget(m_bccEdit, 1);
        headerLayout->addWidget(m_bccRow);
        connect(m_ccButton, &QToolButton::toggled, this,
                [this](const bool visible)
                {
                    setOptionalRecipientVisible(m_ccRow, m_ccButton, visible);
                    if (visible)
                    {
                        m_ccEdit->setFocus();
                    }
                });
        connect(m_bccButton, &QToolButton::toggled, this,
                [this](const bool visible)
                {
                    setOptionalRecipientVisible(m_bccRow, m_bccButton, visible);
                    if (visible)
                    {
                        m_bccEdit->setFocus();
                    }
                });

        auto* subjectRow = new QHBoxLayout();
        auto* subjectLabel = new QLabel(i18nc("@label email subject", "Subject"), headerWidget);
        subjectLabel->setMinimumWidth(52);
        m_subjectEdit = new QLineEdit(headerWidget);
        m_subjectEdit->setPlaceholderText(i18n("Add a subject"));
        subjectRow->addWidget(subjectLabel);
        subjectRow->addWidget(m_subjectEdit, 1);
        headerLayout->addLayout(subjectRow);

        rootLayout->addWidget(headerWidget);

        m_formatToolbar = new QToolBar(this);
        m_formatToolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        rootLayout->addWidget(m_formatToolbar);

        m_editorTabs = new QTabWidget(this);
        m_richTextEdit = new JavelinComposerEdit(m_editorTabs);
        m_richTextEdit->setAcceptDrops(false);
        m_richTextEdit->setAcceptRichText(true);
        m_richTextEdit->document()->setDocumentMargin(8);
        m_previewView = new javelin::gui::messageview::HtmlMessageView(
            m_settings.messageAppearanceSettings(), m_editorTabs);
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
        const QSignalBlocker blocker{m_fromCombo};
        const auto selectedAccountId = QString::fromStdString(m_snapshot.accountId);
        const auto selectedIdentityId = QString::fromStdString(m_snapshot.identityId);
        int selectedIndex = -1;
        int optionCount = 0;

        m_fromCombo->clear();
        for (const auto& connection : m_settings.accounts())
        {
            const auto accountDisplayName =
                connection.displayName.isEmpty() ? connection.loginEmail : connection.displayName;
            for (const auto& cachedAccountId : connection.cachedAccountIds)
            {
                const auto accountId = cachedAccountId.toStdString();
                const auto identitiesResult = m_identityRepository.listByAccount(accountId);
                if (const auto* error =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&identitiesResult))
                {
                    Q_EMIT statusMessageRequested(error->message, 10000);
                    continue;
                }

                const auto& identities =
                    std::get<std::vector<javelin::jmap::domain::Identity>>(identitiesResult);
                std::vector<const javelin::jmap::domain::Identity*> uniqueIdentities;
                std::unordered_map<std::string, std::size_t> identityIndexByEmail;
                for (const auto& identity : identities)
                {
                    if (isWildcardSenderIdentity(identity))
                    {
                        continue;
                    }

                    auto emailKey = QString::fromStdString(identity.email)
                                        .trimmed()
                                        .toCaseFolded()
                                        .toStdString();
                    const auto existing = identityIndexByEmail.find(emailKey);
                    if (existing == identityIndexByEmail.end())
                    {
                        identityIndexByEmail.emplace(std::move(emailKey), uniqueIdentities.size());
                        uniqueIdentities.push_back(&identity);
                    }
                    else if (cachedAccountId == selectedAccountId &&
                             QString::fromStdString(identity.id) == selectedIdentityId)
                    {
                        uniqueIdentities[existing->second] = &identity;
                    }
                }

                const bool hasSenderIdentity = !uniqueIdentities.empty();
                for (const auto* identity : uniqueIdentities)
                {
                    const int index = m_fromCombo->count();
                    m_fromCombo->addItem(identityDisplayText(*identity, accountDisplayName));
                    m_fromCombo->setItemData(index, QString::fromStdString(identity->id),
                                             senderIdentityIdRole);
                    m_fromCombo->setItemData(index, cachedAccountId, senderAccountIdRole);
                    if (cachedAccountId == selectedAccountId &&
                        QString::fromStdString(identity->id) == selectedIdentityId)
                    {
                        selectedIndex = index;
                    }
                    ++optionCount;
                }

                if (!hasSenderIdentity && !m_identityLoadsStarted.contains(accountId) &&
                    !connection.sessionUrl.isEmpty() && !connection.loginEmail.isEmpty() &&
                    !connection.apiKey.isEmpty())
                {
                    m_identityLoadsStarted.insert(accountId);
                    auto task = m_composeCommandPort.loadSenderIdentities(
                        javelin::app::AccountConnectionSettings{
                            .connectionId = connection.id.toStdString(),
                            .revision = connection.revision,
                            .sessionUrl = connection.sessionUrl.toStdString(),
                            .loginEmail = connection.loginEmail.toStdString(),
                            .apiKey = connection.apiKey.toStdString(),
                            .refreshToken = connection.refreshToken.toStdString(),
                            .tokenEndpoint = connection.tokenEndpoint.toStdString(),
                            .oauthClientId = connection.oauthClientId.toStdString(),
                            .oauthIssuer = connection.oauthIssuer.toStdString(),
                            .oauthResource = connection.oauthResource.toStdString(),
                            .oauthScope = connection.oauthScope.toStdString(),
                            .revocationEndpoint = connection.revocationEndpoint.toStdString(),
                        },
                        accountId);
                    QCoro::connect(std::move(task), this,
                                   [this](std::variant<std::vector<javelin::jmap::domain::Identity>,
                                                       javelin::jmap::OperationError>
                                              result)
                                   {
                                       if (const auto* error =
                                               std::get_if<javelin::jmap::OperationError>(&result))
                                       {
                                           Q_EMIT statusMessageRequested(error->message, 10000);
                                           return;
                                       }
                                       loadIdentities();
                                   });
                }
            }
        }

        if (selectedIndex >= 0)
        {
            m_fromCombo->setCurrentIndex(selectedIndex);
        }
        else if (optionCount > 0)
        {
            m_fromCombo->setCurrentIndex(0);
        }
        else
        {
            Q_EMIT statusMessageRequested(
                i18n("No sender identities are available for configured accounts."), 10000);
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
        const QSignalBlocker tabBlocker{m_editorTabs};
        const QSignalBlocker formatBlocker{m_bodyFormatCombo};

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

        m_toEdit->setText(formatAddresses(m_snapshot.to));
        m_ccEdit->setText(formatAddresses(m_snapshot.cc));
        m_bccEdit->setText(formatAddresses(m_snapshot.bcc));
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
        m_bodyFormatCombo->setCurrentIndex(plainTextMode ? plainTextFormatIndex : htmlFormatIndex);
        m_editorTabs->setTabVisible(previewTabIndex, !plainTextMode);
        m_editorTabs->setCurrentIndex(richEditorTabIndex);
        setOptionalRecipientVisible(m_ccRow, m_ccButton, !m_snapshot.cc.empty());
        setOptionalRecipientVisible(m_bccRow, m_bccButton, !m_snapshot.bcc.empty());
        populateAttachments();
        m_syncingUi = false;
    }

    void ComposeTabWidget::populateAttachments()
    {
        m_attachmentScrollArea->setVisible(!m_snapshot.attachments.empty());
        while (m_attachmentStripLayout->count() > 0)
        {
            auto* item = m_attachmentStripLayout->takeAt(0);
            if (auto* widget = item->widget())
            {
                widget->deleteLater();
            }
            delete item;
        }

        for (std::size_t index = 0; index < m_snapshot.attachments.size(); ++index)
        {
            auto* chip = new DraftAttachmentChip(
                m_snapshot.attachments[index],
                m_snapshot.editorMode != javelin::jmap::submission::BodyEditorMode::PlainText,
                [this, index] { removeAttachmentAt(index); }, [this, index](const bool embedded)
                { setAttachmentEmbedded(index, embedded); }, m_attachmentStrip);
            chip->setEnabled(!m_operationInFlight);
            m_attachmentStripLayout->addWidget(chip);
        }
        m_attachmentStripLayout->addStretch(1);
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
        m_snapshot.to = parseAddresses(m_toEdit->text());
        m_snapshot.cc = parseAddresses(m_ccEdit->text());
        m_snapshot.bcc = parseAddresses(m_bccEdit->text());
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
            MessageComposer::TextPart textPart;
            m_richTextEdit->fillComposerTextPart(&textPart);
            const auto html = stableEditorHtml();
            reconcileInlineAttachmentReferences(html);
            m_snapshot.htmlBody = html.toStdString();
            m_snapshot.plainTextBody = textPart.cleanPlainText().toStdString();
            break;
        }
        case javelin::jmap::submission::BodyEditorMode::PlainText:
            m_snapshot.plainTextBody =
                m_richTextEdit->composerControler()->toCleanPlainText().toStdString();
            m_snapshot.htmlBody.clear();
            break;
        }

        ++m_snapshot.revision;
        updateTabTitle();
    }

    void ComposeTabWidget::switchBodyFormat(const int index)
    {
        if (m_syncingUi)
        {
            return;
        }

        const bool plainTextMode = index == plainTextFormatIndex;
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
                QAbstractButton* loseFormatting = warning.addButton(
                    i18nc("@action:button", "Lose Formatting"), QMessageBox::DestructiveRole);
                QPushButton* addMarkup = warning.addButton(
                    i18nc("@action:button", "Add Markup Plain Text"), QMessageBox::AcceptRole);
                QAbstractButton* cancel = warning.addButton(QMessageBox::Cancel);
                warning.setDefaultButton(addMarkup);
                warning.exec();

                if (warning.clickedButton() == cancel || warning.clickedButton() == nullptr)
                {
                    const QSignalBlocker blocker{m_bodyFormatCombo};
                    m_bodyFormatCombo->setCurrentIndex(htmlFormatIndex);
                    return;
                }
                useMarkup = warning.clickedButton() == addMarkup;
                Q_UNUSED(loseFormatting);
            }

            m_syncingUi = true;
            m_richTextEdit->forcePlainTextMarkup(useMarkup);
            m_richTextEdit->switchToPlainText();
            m_snapshot.editorMode = javelin::jmap::submission::BodyEditorMode::PlainText;
            m_syncingUi = false;
        }
        else if (!plainTextMode &&
                 m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::PlainText)
        {
            m_syncingUi = true;
            m_richTextEdit->activateRichText();
            m_snapshot.editorMode = javelin::jmap::submission::BodyEditorMode::RichText;
            m_syncingUi = false;
        }

        m_editorTabs->setCurrentIndex(richEditorTabIndex);
        m_editorTabs->setTabVisible(previewTabIndex, !plainTextMode);

        populateAttachments();
        updateEditorModeUi();
        syncSnapshotFromUi();
        refreshPreview();
        scheduleWorkingCopySave();
    }

    void ComposeTabWidget::setOptionalRecipientVisible(QWidget* row, QToolButton* button,
                                                       const bool visible)
    {
        const QSignalBlocker blocker{button};
        button->setChecked(visible);
        row->setVisible(visible);
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
        if (const auto error = m_composeCommandPort.storeWorkingCopy(m_snapshot))
        {
            Q_EMIT statusMessageRequested(error->message, 10000);
            return;
        }
    }

    void ComposeTabWidget::setBusy(const bool busy)
    {
        m_operationInFlight = busy;
        m_fromCombo->setEnabled(!busy);
        m_bodyFormatCombo->setEnabled(!busy);
        m_toEdit->setEnabled(!busy);
        m_ccEdit->setEnabled(!busy);
        m_bccEdit->setEnabled(!busy);
        m_ccButton->setEnabled(!busy);
        m_bccButton->setEnabled(!busy);
        m_subjectEdit->setEnabled(!busy);
        m_richTextEdit->setEnabled(!busy);
        m_editorTabs->setEnabled(!busy);
        updateEditorModeUi();
        populateAttachments();
    }

    void ComposeTabWidget::updateEditorModeUi()
    {
        const bool richMode =
            m_editorTabs->currentIndex() == richEditorTabIndex &&
            m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::RichText;
        const bool actionsEnabled = !m_operationInFlight && richMode;
        m_formatToolbar->setEnabled(actionsEnabled);
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

    void ComposeTabWidget::addInlineImagePath(const QString& filePath)
    {
        const QFileInfo info{filePath};
        const QImage image{filePath};
        if (!info.exists() || !info.isFile() || image.isNull())
        {
            Q_EMIT statusMessageRequested(i18n("The selected image could not be loaded."), 7000);
            return;
        }

        m_snapshot.attachments.push_back(javelin::jmap::submission::DraftAttachment{
            .localFilePath = filePath.toStdString(),
            .displayName = info.fileName().toStdString(),
            .mediaType = detectedMediaType(filePath).toStdString(),
            .size = static_cast<std::uint64_t>(info.size()),
            .blobId = std::nullopt,
            .inlineDisposition = true,
            .contentId = newContentId(),
            .contentHash = std::nullopt,
        });
        insertEmbeddedImage(m_snapshot.attachments.size() - 1);
        populateAttachments();
        refreshPreview();
        syncSnapshotFromUi();
        scheduleWorkingCopySave();
    }

    void ComposeTabWidget::addPastedInlineImage(const QImage& image)
    {
        if (image.isNull())
        {
            return;
        }

        const auto directory = draftAssetDirectory(m_snapshot.composeSessionId);
        if (!QDir{}.mkpath(directory))
        {
            Q_EMIT statusMessageRequested(i18n("Could not create storage for the pasted image."),
                                          10000);
            return;
        }

        const auto fileName =
            QStringLiteral("pasted-%1.png").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        const auto filePath = QDir{directory}.filePath(fileName);
        if (!image.save(filePath, "PNG"))
        {
            Q_EMIT statusMessageRequested(i18n("Could not save the pasted image."), 10000);
            return;
        }
        addInlineImagePath(filePath);
    }

    void ComposeTabWidget::insertImage()
    {
        const auto filePath = QFileDialog::getOpenFileName(
            this, i18n("Insert Image"), QString{},
            i18n("Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp);;All Files (*)"));
        if (!filePath.isEmpty())
        {
            addInlineImagePath(filePath);
        }
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
        if (index >= m_snapshot.attachments.size() ||
            m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::PlainText ||
            embedded == m_snapshot.attachments[index].inlineDisposition)
        {
            return;
        }

        auto& attachment = m_snapshot.attachments[index];
        attachment.inlineDisposition = embedded;
        if (embedded)
        {
            if (!attachment.contentId.has_value())
            {
                attachment.contentId = newContentId();
            }
            insertEmbeddedImage(index);
        }
        else if (attachment.contentId.has_value())
        {
            removeEmbeddedImageReference(*attachment.contentId);
            attachment.contentId = std::nullopt;
        }

        populateAttachments();
        refreshPreview();
        syncSnapshotFromUi();
        scheduleWorkingCopySave();
    }

    void ComposeTabWidget::insertEmbeddedImage(const std::size_t index)
    {
        if (index >= m_snapshot.attachments.size())
        {
            return;
        }

        const auto& attachment = m_snapshot.attachments[index];
        if (!attachment.contentId.has_value())
        {
            return;
        }

        const QImage image{QString::fromStdString(attachment.localFilePath)};
        if (image.isNull())
        {
            return;
        }

        const auto resourceName = composerEditorResourceName(*attachment.contentId);
        if (!m_richTextEdit->toCleanHtml().contains(resourceName))
        {
            const auto width = std::min(image.width(), 720);
            const auto height = image.width() > 0 ? image.height() * width / image.width() : -1;
            m_richTextEdit->composerControler()->composerImages()->addImageHelper(
                resourceName, image, width, height);
        }
    }

    void ComposeTabWidget::removeEmbeddedImageReference(const std::string& contentId)
    {
        const auto cidUrl = composerContentIdUrl(contentId);
        const QRegularExpression imageTagPattern{
            QStringLiteral("<img\\b[^>]*\\bsrc\\s*=\\s*([\"'])%1\\1[^>]*>")
                .arg(QRegularExpression::escape(cidUrl)),
            QRegularExpression::CaseInsensitiveOption};

        auto html = stableEditorHtml();
        html.remove(imageTagPattern);
        setEditorHtml(html);
    }

    void ComposeTabWidget::setEditorHtml(const QString& html)
    {
        if (m_richTextEdit->textMode() == KPIMTextEdit::RichTextComposer::Plain)
        {
            m_richTextEdit->activateRichText();
        }
        m_richTextEdit->setTextOrHtml(
            htmlForQtDocument(editorHtmlForInlineAttachments(html, m_snapshot.attachments)));
        loadInlineImageResources();
    }

    void ComposeTabWidget::loadInlineImageResources()
    {
        auto* images = m_richTextEdit->composerControler()->composerImages();
        for (const auto& attachment : m_snapshot.attachments)
        {
            if (!attachment.inlineDisposition || !attachment.contentId.has_value() ||
                attachment.localFilePath.empty())
            {
                continue;
            }

            const QImage image{QString::fromStdString(attachment.localFilePath)};
            if (image.isNull())
            {
                continue;
            }
            const auto resourceName = composerEditorResourceName(*attachment.contentId);
            images->loadImage(image, resourceName, resourceName);
        }
    }

    QString ComposeTabWidget::stableEditorHtml()
    {
        return stableHtmlForInlineAttachments(m_richTextEdit->toCleanHtml(),
                                              m_snapshot.attachments);
    }

    void ComposeTabWidget::reconcileInlineAttachmentReferences(const QString& html)
    {
        if (reconcileInlineAttachments(m_snapshot.attachments, html))
        {
            populateAttachments();
        }
    }

    void ComposeTabWidget::startSaveDraft(const bool closeAfterSave)
    {
        if (m_operationInFlight)
        {
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

        if (m_autosaveTimer->isActive())
        {
            m_autosaveTimer->stop();
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

    void ComposeTabWidget::startSend()
    {
        if (m_operationInFlight)
        {
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

        if (m_snapshot.to.empty())
        {
            Q_EMIT statusMessageRequested(i18n("Add at least one recipient before sending."), 7000);
            return;
        }

        setBusy(true);
        Q_EMIT statusMessageRequested(i18n("Sending message..."), 5000);
        auto task = m_composeCommandPort.send(*settings, m_snapshot);
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
