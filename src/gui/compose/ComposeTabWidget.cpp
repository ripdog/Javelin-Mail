#include "gui/compose/ComposeTabWidget.h"

#include "app/ComposeService.h"
#include "gui/messageview/HtmlMessageView.h"
#include "gui/settings/PreferencesDialog.h"
#include "jmap/cache/IdentityRepository.h"
#include "jmap/contacts/ContactIdentityLookup.h"

#include <QCoroTask>

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCompleter>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QMimeDatabase>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStringListModel>
#include <QStyle>
#include <QTabWidget>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextImageFormat>
#include <QTextListFormat>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
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
        constexpr auto htmlSourceTabIndex = 1;
        constexpr auto previewTabIndex = 2;
        constexpr auto senderIdentityIdRole = Qt::UserRole;
        constexpr auto senderAccountIdRole = Qt::UserRole + 1;

        [[nodiscard]] QString defaultTitleForMode(const javelin::jmap::submission::ComposeMode mode)
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

        [[nodiscard]] QString
        attachmentItemText(const javelin::jmap::submission::DraftAttachment& attachment)
        {
            const auto displayName =
                !attachment.displayName.empty()
                    ? QString::fromStdString(attachment.displayName)
                    : QFileInfo{QString::fromStdString(attachment.localFilePath)}.fileName();
            const auto mediaType = attachment.mediaType.empty()
                                       ? QStringLiteral("attachment")
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
            return QStringLiteral("Attachment");
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

        [[nodiscard]] QString sanitizedPastedHtml(QString html)
        {
            static const QRegularExpression styleElement{
                QStringLiteral("<style\\b[^>]*>.*?</style>"),
                QRegularExpression::CaseInsensitiveOption |
                    QRegularExpression::DotMatchesEverythingOption};
            static const QRegularExpression styleAttribute{
                QStringLiteral("\\sstyle\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s>]+)"),
                QRegularExpression::CaseInsensitiveOption};
            static const QRegularExpression classAttribute{
                QStringLiteral("\\sclass\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s>]+)"),
                QRegularExpression::CaseInsensitiveOption};
            static const QRegularExpression fontAttribute{
                QStringLiteral("\\s(?:face|color|size)\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s>]+)"),
                QRegularExpression::CaseInsensitiveOption};
            static const QRegularExpression fontOpenTag{QStringLiteral("<font\\b[^>]*>"),
                                                        QRegularExpression::CaseInsensitiveOption};
            static const QRegularExpression fontCloseTag{QStringLiteral("</font\\s*>"),
                                                         QRegularExpression::CaseInsensitiveOption};

            html.remove(styleElement);
            html.remove(styleAttribute);
            html.remove(classAttribute);
            html.remove(fontAttribute);
            html.remove(fontOpenTag);
            html.remove(fontCloseTag);
            return html;
        }

        class RichTextComposeEdit : public QTextEdit
        {
          public:
            using QTextEdit::QTextEdit;

          protected:
            void insertFromMimeData(const QMimeData* source) override
            {
                if (source->hasHtml())
                {
                    QMimeData sanitized;
                    sanitized.setHtml(sanitizedPastedHtml(source->html()));
                    if (source->hasText())
                    {
                        sanitized.setText(source->text());
                    }
                    QTextEdit::insertFromMimeData(&sanitized);
                    return;
                }

                QTextEdit::insertFromMimeData(source);
            }
        };

        class DraftAttachmentChip : public QFrame
        {
          public:
            DraftAttachmentChip(const javelin::jmap::submission::DraftAttachment& attachment,
                                std::function<void()> removeAction,
                                std::function<void(bool)> embedAction, QWidget* parent = nullptr)
                : QFrame(parent), m_removeAction(std::move(removeAction)),
                  m_embedAction(std::move(embedAction))
            {
                setObjectName(QStringLiteral("draftAttachmentChip"));
                setToolTip(attachmentItemText(attachment));
                setStyleSheet(QStringLiteral(
                    "#draftAttachmentChip { background: rgba(255, 255, 255, 0.06); border: 1px "
                    "solid rgba(255, 255, 255, 0.08); border-radius: 6px; }"));

                auto* layout = new QHBoxLayout(this);
                layout->setContentsMargins(8, 4, 5, 4);
                layout->setSpacing(6);

                auto* iconLabel = new QLabel(this);
                iconLabel->setPixmap(attachmentIcon(attachment).pixmap(16, 16));
                iconLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

                auto* nameLabel = new QLabel(attachmentDisplayName(attachment), this);
                nameLabel->setMinimumWidth(80);
                nameLabel->setMaximumWidth(220);
                nameLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

                auto* sizeLabel = new QLabel(attachmentSizeLabel(attachment.size), this);
                sizeLabel->setStyleSheet(QStringLiteral("color: #c2c6cf;"));
                sizeLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

                if (isImageAttachment(attachment))
                {
                    auto* attachRadio = new QRadioButton(QStringLiteral("Attach"), this);
                    auto* embedRadio = new QRadioButton(QStringLiteral("Embed"), this);
                    attachRadio->setChecked(!attachment.inlineDisposition);
                    embedRadio->setChecked(attachment.inlineDisposition);
                    attachRadio->setToolTip(QStringLiteral("Send this image as an attachment"));
                    embedRadio->setToolTip(QStringLiteral("Show this image in the message body"));
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
                removeButton->setToolTip(QStringLiteral("Remove attachment"));
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

        [[nodiscard]] std::string plainTextFromHtml(const QString& html)
        {
            QTextDocument document;
            document.setHtml(html);
            return document.toPlainText().toStdString();
        }

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
        liveSettings(const std::string_view accountId, QString* errorMessage = nullptr)
        {
            const auto settings = javelin::gui::settings::PreferencesDialog::loadSettingsForAccount(
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

            return javelin::app::AccountConnectionSettings{
                .sessionUrl = settings.sessionUrl.toStdString(),
                .loginEmail = settings.loginEmail.toStdString(),
                .apiKey = settings.apiKey.toStdString(),
            };
        }

    } // namespace

    ComposeTabWidget::ComposeTabWidget(
        javelin::app::ComposeService& composeService,
        javelin::jmap::cache::IdentityRepository& identityRepository,
        javelin::jmap::contacts::ContactIdentityLookup& contactIdentityLookup,
        javelin::jmap::submission::DraftSnapshot snapshot, QWidget* parent)
        : QWidget(parent), m_composeService(composeService),
          m_identityRepository(identityRepository), m_contactIdentityLookup(contactIdentityLookup),
          m_snapshot(std::move(snapshot))
    {
        setAcceptDrops(true);
        setupUi();
        connect(&m_contactIdentityLookup,
                &javelin::jmap::contacts::ContactIdentityLookup::contactDataChanged, this,
                &ComposeTabWidget::setupContactCompletion);
        setupContactCompletion();
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
                    if (m_syncingUi ||
                        m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::RawHtml)
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
                    if (m_syncingUi ||
                        m_snapshot.editorMode != javelin::jmap::submission::BodyEditorMode::RawHtml)
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
                        m_snapshot.editorMode != javelin::jmap::submission::BodyEditorMode::RawHtml)
                    {
                        syncHtmlSourceFromRichText();
                        m_snapshot.editorMode = javelin::jmap::submission::BodyEditorMode::RawHtml;
                    }
                    else if (index == richEditorTabIndex &&
                             m_snapshot.editorMode ==
                                 javelin::jmap::submission::BodyEditorMode::RawHtml)
                    {
                        syncRichTextFromHtmlSource();
                        m_snapshot.editorMode = javelin::jmap::submission::BodyEditorMode::RichText;
                    }

                    updateEditorModeUi();
                    syncSnapshotFromUi();
                    refreshPreview();
                    scheduleWorkingCopySave();
                });

        connect(m_addAttachmentButton, &QPushButton::clicked, this,
                &ComposeTabWidget::addAttachments);
        connect(m_saveDraftButton, &QPushButton::clicked, this, [this] { startSaveDraft(false); });
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

    void ComposeTabWidget::setupContactCompletion()
    {
        const auto result = m_contactIdentityLookup.suggestions();
        const auto* contacts =
            std::get_if<std::vector<javelin::jmap::contacts::ContactIdentity>>(&result);
        if (contacts == nullptr)
        {
            return;
        }
        QStringList values;
        values.reserve(static_cast<qsizetype>(contacts->size()));
        for (const auto& contact : *contacts)
        {
            QString name = QString::fromStdString(contact.displayName);
            if (contact.organization.has_value() && *contact.organization != contact.displayName)
            {
                name += QStringLiteral(" — %1").arg(QString::fromStdString(*contact.organization));
            }
            values.push_back(
                QStringLiteral("%1 <%2>").arg(name, QString::fromStdString(contact.email)));
        }

        if (m_contactCompletionModel != nullptr)
        {
            m_contactCompletionModel->setStringList(values);
            return;
        }

        m_contactCompletionModel = new QStringListModel(values, this);
        for (auto* edit : {m_toEdit, m_ccEdit, m_bccEdit})
        {
            auto* completer = new QCompleter(m_contactCompletionModel, edit);
            completer->setWidget(edit);
            completer->setCaseSensitivity(Qt::CaseInsensitive);
            completer->setCompletionMode(QCompleter::PopupCompletion);
            completer->setFilterMode(Qt::MatchContains);
            connect(edit, &QLineEdit::textEdited, completer,
                    [edit, completer](const QString& text)
                    {
                        const qsizetype separator = std::max(text.lastIndexOf(QLatin1Char(',')),
                                                             text.lastIndexOf(QLatin1Char(';')));
                        const QString token = text.sliced(separator + 1).trimmed();
                        if (token.isEmpty())
                        {
                            completer->popup()->hide();
                            return;
                        }
                        completer->setCompletionPrefix(token);
                        completer->complete();
                    });
            connect(completer, qOverload<const QString&>(&QCompleter::activated), edit,
                    [edit](const QString& completion)
                    {
                        const QString text = edit->text();
                        const qsizetype separator = std::max(text.lastIndexOf(QLatin1Char(',')),
                                                             text.lastIndexOf(QLatin1Char(';')));
                        const QString prefix = separator >= 0
                                                   ? text.left(separator + 1) + QStringLiteral(" ")
                                                   : QString{};
                        edit->setText(prefix + completion);
                        edit->setCursorPosition(static_cast<int>(edit->text().size()));
                    });
        }
    }

    void ComposeTabWidget::setupUi()
    {
        setObjectName(QStringLiteral("composeTab"));
        setStyleSheet(QStringLiteral(
            "#composeTab { background: #1d2026; }"
            "#composeHeader { background: #232833; border: 1px solid #333c4b; border-radius: 14px; "
            "}"
            "QLineEdit, QComboBox, QPlainTextEdit, QTextEdit {"
            "  background: #161a20; color: #eef2f7; border: 1px solid #394354; border-radius: 8px;"
            "}"
            "QLineEdit, QComboBox { padding: 6px 8px; }"
            "QToolBar { background: #202632; border: 1px solid #394354; border-radius: 10px; "
            "spacing: 4px; }"
            "QPushButton { background: #2b3341; color: #eef2f7; border: 1px solid #43506a; "
            "border-radius: 9px; padding: 7px 12px; }"
            "QPushButton:hover { background: #334055; }"
            "QPushButton:disabled { color: #8a93a5; background: #232833; }"
            "QTabWidget::pane { border: 1px solid #333c4b; border-radius: 12px; background: "
            "#20242d; }"
            "QTabBar::tab { background: #262c38; color: #ced7e4; padding: 8px 14px; "
            "border-top-left-radius: 8px; border-top-right-radius: 8px; }"
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
        headingRow->addWidget(titleLabel);
        headingRow->addStretch(1);
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
        m_richTextEdit = new RichTextComposeEdit(m_editorTabs);
        m_richTextEdit->setAcceptDrops(false);
        m_richTextEdit->setAcceptRichText(true);
        m_richTextEdit->document()->setDocumentMargin(14);
        m_htmlSourceEdit = new QPlainTextEdit(m_editorTabs);
        m_htmlSourceEdit->setAcceptDrops(false);
        m_htmlSourceEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
        m_previewView = new javelin::gui::messageview::HtmlMessageView(m_editorTabs);
        m_previewView->setAcceptDrops(false);
        m_previewView->setRemoteContentEnabled(false);
        m_editorTabs->addTab(m_richTextEdit, QStringLiteral("Compose"));
        m_editorTabs->addTab(m_htmlSourceEdit, QStringLiteral("HTML"));
        m_editorTabs->addTab(m_previewView, QStringLiteral("Preview"));
        rootLayout->addWidget(m_editorTabs, 1);

        auto* attachmentRow = new QHBoxLayout();
        attachmentRow->setSpacing(8);
        m_addAttachmentButton = new QPushButton(QStringLiteral("Attach File"), this);
        m_addAttachmentButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        attachmentRow->addWidget(m_addAttachmentButton);

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
        m_attachmentScrollArea->setFixedHeight(m_addAttachmentButton->sizeHint().height() + 4);
        attachmentRow->addWidget(m_attachmentScrollArea, 1);
        rootLayout->addLayout(attachmentRow);

        auto* footerRow = new QHBoxLayout();
        m_saveDraftButton = new QPushButton(QStringLiteral("Save Draft"), this);
        m_sendButton = new QPushButton(QStringLiteral("Send"), this);
        m_closeButton = new QPushButton(QStringLiteral("Close"), this);
        footerRow->addStretch(1);
        footerRow->addWidget(m_saveDraftButton);
        footerRow->addWidget(m_sendButton);
        footerRow->addWidget(m_closeButton);
        rootLayout->addLayout(footerRow);
    }

    void ComposeTabWidget::createToolbarActions()
    {
        m_boldAction = m_formatToolbar->addAction(
            QIcon::fromTheme(QStringLiteral("format-text-bold")), QStringLiteral("Bold"));
        m_boldAction->setCheckable(true);
        connect(m_boldAction, &QAction::triggered, this, &ComposeTabWidget::toggleBold);

        m_italicAction = m_formatToolbar->addAction(
            QIcon::fromTheme(QStringLiteral("format-text-italic")), QStringLiteral("Italic"));
        m_italicAction->setCheckable(true);
        connect(m_italicAction, &QAction::triggered, this, &ComposeTabWidget::toggleItalic);

        m_underlineAction = m_formatToolbar->addAction(
            QIcon::fromTheme(QStringLiteral("format-text-underline")), QStringLiteral("Underline"));
        m_underlineAction->setCheckable(true);
        connect(m_underlineAction, &QAction::triggered, this, &ComposeTabWidget::toggleUnderline);

        m_strikethroughAction = m_formatToolbar->addAction(
            QIcon::fromTheme(QStringLiteral("format-text-strikethrough")),
            QStringLiteral("Strike"));
        m_strikethroughAction->setCheckable(true);
        connect(m_strikethroughAction, &QAction::triggered, this,
                &ComposeTabWidget::toggleStrikethrough);

        m_formatToolbar->addSeparator();

        auto* bulletAction = m_formatToolbar->addAction(
            QIcon::fromTheme(QStringLiteral("format-list-unordered")), QStringLiteral("Bullets"));
        connect(bulletAction, &QAction::triggered, this, &ComposeTabWidget::insertBulletList);

        auto* numberedAction = m_formatToolbar->addAction(
            QIcon::fromTheme(QStringLiteral("format-list-ordered")), QStringLiteral("Numbering"));
        connect(numberedAction, &QAction::triggered, this, &ComposeTabWidget::insertNumberedList);

        auto* linkAction = m_formatToolbar->addAction(
            QIcon::fromTheme(QStringLiteral("insert-link")), QStringLiteral("Link"));
        connect(linkAction, &QAction::triggered, this, &ComposeTabWidget::insertLink);

        m_formatToolbar->addSeparator();

        auto* alignLeftAction = m_formatToolbar->addAction(
            QIcon::fromTheme(QStringLiteral("format-justify-left")), QStringLiteral("Left"));
        connect(alignLeftAction, &QAction::triggered, this, &ComposeTabWidget::alignLeft);

        auto* alignCenterAction = m_formatToolbar->addAction(
            QIcon::fromTheme(QStringLiteral("format-justify-center")), QStringLiteral("Center"));
        connect(alignCenterAction, &QAction::triggered, this, &ComposeTabWidget::alignCenter);

        auto* alignRightAction = m_formatToolbar->addAction(
            QIcon::fromTheme(QStringLiteral("format-justify-right")), QStringLiteral("Right"));
        connect(alignRightAction, &QAction::triggered, this, &ComposeTabWidget::alignRight);

        auto* clearAction = m_formatToolbar->addAction(
            QIcon::fromTheme(QStringLiteral("edit-clear-format")), QStringLiteral("Clear"));
        connect(clearAction, &QAction::triggered, this, &ComposeTabWidget::clearFormatting);
    }

    void ComposeTabWidget::loadIdentities()
    {
        const QSignalBlocker blocker{m_fromCombo};
        const auto selectedAccountId = QString::fromStdString(m_snapshot.accountId);
        const auto selectedIdentityId = QString::fromStdString(m_snapshot.identityId);
        int selectedIndex = -1;
        int optionCount = 0;

        m_fromCombo->clear();
        for (const auto& connection : javelin::gui::settings::PreferencesDialog::loadAccounts())
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
                    auto task = m_composeService.loadSenderIdentities(
                        javelin::app::AccountConnectionSettings{
                            .sessionUrl = connection.sessionUrl.toStdString(),
                            .loginEmail = connection.loginEmail.toStdString(),
                            .apiKey = connection.apiKey.toStdString(),
                        },
                        accountId);
                    QCoro::connect(
                        std::move(task), this,
                        [this](std::variant<std::vector<javelin::jmap::domain::Identity>,
                                            javelin::jmap::LiveRefreshError>
                                   result)
                        {
                            if (const auto* error =
                                    std::get_if<javelin::jmap::LiveRefreshError>(&result))
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
                QStringLiteral("No sender identities are available for configured accounts."),
                10000);
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
        m_richTextEdit->setHtml(QString::fromStdString(m_snapshot.htmlBody));
        m_htmlSourceEdit->setPlainText(QString::fromStdString(m_snapshot.htmlBody));
        m_editorTabs->setCurrentIndex(m_snapshot.editorMode ==
                                              javelin::jmap::submission::BodyEditorMode::RawHtml
                                          ? htmlSourceTabIndex
                                          : richEditorTabIndex);
        populateAttachments();
        m_syncingUi = false;
    }

    void ComposeTabWidget::populateAttachments()
    {
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
                m_snapshot.attachments[index], [this, index] { removeAttachmentAt(index); },
                [this, index](const bool embedded) { setAttachmentEmbedded(index, embedded); },
                m_attachmentStrip);
            chip->setEnabled(!m_operationInFlight);
            m_attachmentStripLayout->addWidget(chip);
        }
        m_attachmentStripLayout->addStretch(1);
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
        populateAttachments();
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
    }

    void ComposeTabWidget::updateTabTitle()
    {
        Q_EMIT titleChanged(tabTitle());
    }

    void ComposeTabWidget::addAttachments()
    {
        const auto filePaths = QFileDialog::getOpenFileNames(this, QStringLiteral("Attach Files"));
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
            });
        }

        populateAttachments();
        scheduleWorkingCopySave();
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

        const auto cidUrl =
            QStringLiteral("cid:%1").arg(QString::fromStdString(*attachment.contentId));
        if (m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::RawHtml)
        {
            auto html = m_htmlSourceEdit->toPlainText();
            if (!html.contains(cidUrl))
            {
                html.append(QStringLiteral("<p><img src=\"%1\" alt=\"%2\"></p>")
                                .arg(cidUrl, attachmentDisplayName(attachment).toHtmlEscaped()));
                m_htmlSourceEdit->setPlainText(html);
            }
            return;
        }

        if (!attachment.localFilePath.empty())
        {
            const QImage image{QString::fromStdString(attachment.localFilePath)};
            if (!image.isNull())
            {
                m_richTextEdit->document()->addResource(QTextDocument::ImageResource, QUrl{cidUrl},
                                                        image);
            }
        }

        if (!m_richTextEdit->document()->toHtml().contains(cidUrl))
        {
            QTextImageFormat imageFormat;
            imageFormat.setName(cidUrl);
            imageFormat.setToolTip(attachmentDisplayName(attachment));
            imageFormat.setWidth(720.0);
            auto cursor = m_richTextEdit->textCursor();
            cursor.insertBlock();
            cursor.insertImage(imageFormat);
            cursor.insertBlock();
            m_richTextEdit->setTextCursor(cursor);
        }
    }

    void ComposeTabWidget::removeEmbeddedImageReference(const std::string& contentId)
    {
        const auto cidUrl = QStringLiteral("cid:%1").arg(QString::fromStdString(contentId));
        const QRegularExpression imageTagPattern{
            QStringLiteral("<img\\b[^>]*\\bsrc\\s*=\\s*([\"'])%1\\1[^>]*>")
                .arg(QRegularExpression::escape(cidUrl)),
            QRegularExpression::CaseInsensitiveOption};

        if (m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::RawHtml)
        {
            auto html = m_htmlSourceEdit->toPlainText();
            html.remove(imageTagPattern);
            m_htmlSourceEdit->setPlainText(html);
            return;
        }

        auto html = m_richTextEdit->document()->toHtml();
        html.remove(imageTagPattern);
        m_richTextEdit->setHtml(html);
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

        syncSnapshotFromUi();
        QString errorMessage;
        const auto settings = liveSettings(m_snapshot.accountId, &errorMessage);
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
                                javelin::jmap::LiveRefreshError>
                       result)
            {
                setBusy(false);
                if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
                {
                    Q_EMIT statusMessageRequested(error->message, 10000);
                    if (error->requiresUserIntervention)
                    {
                        Q_EMIT userInterventionRequired(error->message);
                    }
                    m_closeAfterSave = false;
                    return;
                }

                const auto& summary = std::get<javelin::jmap::submission::DraftSaveSummary>(result);
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
                    if (const auto error = m_composeService.discard(m_snapshot.composeSessionId))
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
        const auto settings = liveSettings(m_snapshot.accountId, &errorMessage);
        if (!settings.has_value())
        {
            Q_EMIT statusMessageRequested(errorMessage, 10000);
            Q_EMIT userInterventionRequired(errorMessage);
            return;
        }

        if (m_snapshot.to.empty())
        {
            Q_EMIT statusMessageRequested(
                QStringLiteral("Add at least one recipient before sending."), 7000);
            return;
        }

        setBusy(true);
        Q_EMIT statusMessageRequested(QStringLiteral("Sending message..."), 5000);
        auto task = m_composeService.send(*settings, m_snapshot);
        QCoro::connect(
            std::move(task), this,
            [this](std::variant<javelin::jmap::submission::SendSummary,
                                javelin::jmap::LiveRefreshError>
                       result)
            {
                setBusy(false);
                if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
                {
                    Q_EMIT statusMessageRequested(error->message, 10000);
                    if (error->requiresUserIntervention)
                    {
                        Q_EMIT userInterventionRequired(error->message);
                    }
                    return;
                }

                const auto& summary = std::get<javelin::jmap::submission::SendSummary>(result);
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
        const auto url =
            QInputDialog::getText(this, QStringLiteral("Insert Link"), QStringLiteral("URL"),
                                  QLineEdit::Normal, QStringLiteral("https://"), &accepted);
        if (!accepted || url.trimmed().isEmpty())
        {
            return;
        }

        QTextCursor cursor = m_richTextEdit->textCursor();
        const auto label = existingSelection.isEmpty() ? url.trimmed() : existingSelection;
        cursor.insertHtml(QStringLiteral("<a href=\"%1\">%2</a>")
                              .arg(url.toHtmlEscaped(), label.toHtmlEscaped()));
    }

} // namespace javelin::gui::compose
