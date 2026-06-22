#include "gui/messageview/MessageViewContainer.h"
#include "gui/messageview/HtmlMessageView.h"

#include <QApplication>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QScrollArea>
#include <QSettings>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStringList>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <vector>

namespace javelin::gui::messageview
{
    namespace
    {
        constexpr auto remoteContentGroup = "remoteContent";
        constexpr auto allowedSendersKey = "allowedSenders";
        constexpr auto allowedDomainsKey = "allowedDomains";

        [[nodiscard]] QString
        attachmentName(const javelin::jmap::cache::MessageAttachment& attachment)
        {
            return QString::fromStdString(attachment.name.value_or(attachment.partId));
        }

        [[nodiscard]] std::optional<std::string> renderedBodyKey(
            const std::optional<javelin::jmap::cache::MessageViewSnapshot>& snapshot)
        {
            if (!snapshot.has_value())
            {
                return std::nullopt;
            }

            if (snapshot->htmlBody.has_value())
            {
                if (snapshot->htmlRenderDocument.has_value())
                {
                    return snapshot->htmlRenderDocument->html;
                }

                return snapshot->htmlBody->value;
            }

            if (snapshot->plainTextBody.has_value())
            {
                return snapshot->plainTextBody->value;
            }

            return std::nullopt;
        }

        [[nodiscard]] QString
        attachmentSizeLabel(const javelin::jmap::cache::MessageAttachment& attachment)
        {
            return QLocale{}.formattedDataSize(static_cast<qint64>(attachment.size));
        }

        void makeLabelSelectable(QLabel* label)
        {
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            label->setCursor(Qt::IBeamCursor);
            label->setFocusPolicy(Qt::NoFocus);
            label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
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

        void addRemoteContentAllowListValue(const QLatin1StringView key, QString value)
        {
            value = value.trimmed().toLower();
            if (value.isEmpty())
            {
                return;
            }

            auto values = remoteContentAllowList(key);
            if (!values.contains(value, Qt::CaseInsensitive))
            {
                values.push_back(value);
                saveRemoteContentAllowList(key, values);
            }
        }

        [[nodiscard]] QIcon
        attachmentIcon(const javelin::jmap::cache::MessageAttachment& attachment)
        {
            const auto fileName = attachmentName(attachment);
            const QFileInfo fileInfo(fileName);
            QFileIconProvider iconProvider;
            auto icon = iconProvider.icon(fileInfo);
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

        class AttachmentTile : public QFrame
        {
          public:
            AttachmentTile(const javelin::jmap::cache::MessageAttachment& attachment,
                           std::function<void()> openAction, std::function<void()> saveAction,
                           QWidget* parent = nullptr)
                : QFrame(parent), m_openAction(std::move(openAction)),
                  m_saveAction(std::move(saveAction))
            {
                setToolTip(attachmentName(attachment));
                setCursor(Qt::PointingHandCursor);
                setFrameStyle(QFrame::NoFrame);
                setObjectName(QStringLiteral("attachmentTile"));
                setStyleSheet(QStringLiteral(
                    "#attachmentTile { background: rgba(255, 255, 255, 0.06); border: 1px solid "
                    "rgba(255, 255, 255, 0.08); border-radius: 6px; }"
                    "#attachmentTile:hover { background: rgba(255, 255, 255, 0.1); }"));

                auto* layout = new QHBoxLayout(this);
                layout->setContentsMargins(8, 6, 8, 6);
                layout->setSpacing(8);

                auto* iconLabel = new QLabel(this);
                iconLabel->setPixmap(attachmentIcon(attachment).pixmap(18, 18));
                iconLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

                auto* nameLabel = new QLabel(attachmentName(attachment), this);
                nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                nameLabel->setWordWrap(false);
                nameLabel->setToolTip(attachmentName(attachment));

                auto* sizeLabel = new QLabel(attachmentSizeLabel(attachment), this);
                sizeLabel->setStyleSheet(QStringLiteral("color: #c2c6cf;"));
                sizeLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

                layout->addWidget(iconLabel);
                layout->addWidget(nameLabel, 1);
                layout->addWidget(sizeLabel);
            }

          protected:
            void mousePressEvent(QMouseEvent* event) override
            {
                if (event->button() == Qt::LeftButton)
                {
                    showMenu(event->globalPosition().toPoint());
                    event->accept();
                    return;
                }

                QFrame::mousePressEvent(event);
            }

            void contextMenuEvent(QContextMenuEvent* event) override
            {
                showMenu(event->globalPos());
                event->accept();
            }

          private:
            void showMenu(const QPoint& globalPos)
            {
                QMenu menu(this);
                auto* open = menu.addAction(QStringLiteral("Open"));
                auto* save = menu.addAction(QStringLiteral("Save"));
                const QAction* chosen = menu.exec(globalPos);
                if (chosen == open && m_openAction)
                {
                    m_openAction();
                }
                else if (chosen == save && m_saveAction)
                {
                    m_saveAction();
                }
            }

            std::function<void()> m_openAction;
            std::function<void()> m_saveAction;
        };

        class MessagePreviewTile : public QFrame
        {
          public:
            MessagePreviewTile(const javelin::jmap::cache::MessageListItem& message,
                               std::function<void()> activateAction, QWidget* parent = nullptr)
                : QFrame(parent), m_activateAction(std::move(activateAction))
            {
                setCursor(Qt::PointingHandCursor);
                setFrameStyle(QFrame::NoFrame);
                setObjectName(QStringLiteral("messagePreviewTile"));
                setStyleSheet(QStringLiteral(
                    "#messagePreviewTile { background: transparent; border: none; }"
                    "#messagePreviewTile:hover { background: rgba(255, 255, 255, 0.06); }"));

                auto* layout = new QVBoxLayout(this);
                layout->setContentsMargins(8, 8, 8, 8);
                layout->setSpacing(4);

                const auto subject = message.subject.has_value()
                                         ? QString::fromStdString(*message.subject)
                                         : QStringLiteral("(no subject)");
                auto* subjectLabel = new QLabel(subject, this);
                subjectLabel->setWordWrap(true);
                auto subjectFont = subjectLabel->font();
                subjectFont.setBold(true);
                subjectFont.setPointSize(subjectFont.pointSize() + 1);
                subjectLabel->setFont(subjectFont);

                const auto preview = message.preview.has_value()
                                         ? QString::fromStdString(*message.preview)
                                         : QStringLiteral("(no preview available)");
                auto* previewLabel = new QLabel(preview, this);
                previewLabel->setWordWrap(true);
                previewLabel->setMaximumHeight(previewLabel->fontMetrics().lineSpacing() * 2 + 6);
                previewLabel->setStyleSheet(QStringLiteral("color: #c5cad3;"));

                layout->addWidget(subjectLabel);
                layout->addWidget(previewLabel);
            }

          protected:
            void mouseReleaseEvent(QMouseEvent* event) override
            {
                if (event->button() == Qt::LeftButton &&
                    rect().contains(event->position().toPoint()))
                {
                    if (m_activateAction)
                    {
                        m_activateAction();
                    }
                    event->accept();
                    return;
                }

                QFrame::mouseReleaseEvent(event);
            }

          private:
            std::function<void()> m_activateAction;
        };

        [[nodiscard]] std::vector<const javelin::jmap::cache::MessageAttachment*>
        visibleAttachments(const std::optional<javelin::jmap::cache::MessageViewSnapshot>& snapshot)
        {
            std::vector<const javelin::jmap::cache::MessageAttachment*> attachments;
            if (!snapshot.has_value())
            {
                return attachments;
            }

            attachments.reserve(snapshot->attachments.size());
            for (const auto& attachment : snapshot->attachments)
            {
                const bool isEmbeddedInline =
                    attachment.cid.has_value() && attachment.disposition.has_value() &&
                    std::ranges::equal(*attachment.disposition, std::string_view{"inline"},
                                       [](const char left, const char right)
                                       {
                                           return std::tolower(static_cast<unsigned char>(left)) ==
                                                  std::tolower(static_cast<unsigned char>(right));
                                       });
                if (!isEmbeddedInline)
                {
                    attachments.push_back(&attachment);
                }
            }

            return attachments;
        }

    } // namespace

    MessageViewContainer::MessageViewContainer(QWidget* parent) : QWidget(parent)
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 8, 8, 0);
        layout->setSpacing(8);

        auto* headerWidget = new QWidget(this);
        auto* headerLayout = new QVBoxLayout(headerWidget);
        headerLayout->setContentsMargins(0, 0, 0, 0);
        headerLayout->setSpacing(4);

        m_titleLabel = new QLabel(this);
        m_titleLabel->setObjectName(QStringLiteral("messageViewTitle"));
        m_titleLabel->setWordWrap(true);
        makeLabelSelectable(m_titleLabel);

        auto titleFont = m_titleLabel->font();
        titleFont.setPointSize(titleFont.pointSize() + 4);
        titleFont.setBold(true);
        m_titleLabel->setFont(titleFont);

        m_detailLabel = new QLabel(this);
        m_detailLabel->setWordWrap(true);
        makeLabelSelectable(m_detailLabel);

        headerLayout->addWidget(m_titleLabel);
        headerLayout->addWidget(m_detailLabel);

        m_bodyControlsWidget = new QWidget(this);
        auto* buttonLayout = new QHBoxLayout(m_bodyControlsWidget);
        buttonLayout->setContentsMargins(0, 0, 0, 0);
        buttonLayout->setSpacing(6);

        m_remoteContentStatusLabel = new QLabel(m_bodyControlsWidget);
        m_remoteContentStatusLabel->setWordWrap(true);
        makeLabelSelectable(m_remoteContentStatusLabel);

        m_permitSenderRemoteContentButton = new QToolButton(m_bodyControlsWidget);
        m_permitSenderRemoteContentButton->setText(QStringLiteral("Always load sender"));
        connect(m_permitSenderRemoteContentButton, &QToolButton::clicked, this,
                &MessageViewContainer::permitRemoteContentForCurrentSender);

        m_permitDomainRemoteContentButton = new QToolButton(m_bodyControlsWidget);
        m_permitDomainRemoteContentButton->setText(QStringLiteral("Always load domain"));
        connect(m_permitDomainRemoteContentButton, &QToolButton::clicked, this,
                &MessageViewContainer::permitRemoteContentForCurrentDomain);

        m_remoteContentButton = new QToolButton(m_bodyControlsWidget);
        m_remoteContentButton->setText(QStringLiteral("Load remote content"));
        m_remoteContentButton->setCheckable(true);
        connect(m_remoteContentButton, &QToolButton::clicked, this,
                [this](const bool checked)
                {
                    m_htmlView->setRemoteContentEnabled(checked);
                    updateRemoteContentButton();
                });

        buttonLayout->addWidget(m_remoteContentStatusLabel, 1);
        buttonLayout->addWidget(m_permitSenderRemoteContentButton);
        buttonLayout->addWidget(m_permitDomainRemoteContentButton);
        buttonLayout->addWidget(m_remoteContentButton);

        m_bodyStack = new QStackedWidget(this);
        m_bodyStack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

        m_placeholderPanel = new QWidget(this);
        auto* placeholderOuterLayout = new QVBoxLayout(m_placeholderPanel);
        placeholderOuterLayout->setContentsMargins(0, 12, 0, 12);
        placeholderOuterLayout->addStretch(1);

        auto* placeholderCard = new QWidget(m_placeholderPanel);
        placeholderCard->setObjectName(QStringLiteral("messagePlaceholderCard"));
        placeholderCard->setMinimumWidth(280);
        placeholderCard->setMaximumWidth(520);
        placeholderCard->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        placeholderCard->setStyleSheet(QStringLiteral("#messagePlaceholderCard {"
                                                      " background: #1f2126;"
                                                      " border: 1px solid #393d46;"
                                                      " border-radius: 16px;"
                                                      "}"
                                                      "#messagePlaceholderCard QLabel {"
                                                      " color: #e6e9ef;"
                                                      "}"));

        auto* placeholderCardLayout = new QVBoxLayout(placeholderCard);
        placeholderCardLayout->setContentsMargins(24, 22, 24, 22);
        placeholderCardLayout->setSpacing(10);

        m_placeholderTitleLabel = new QLabel(placeholderCard);
        auto placeholderTitleFont = m_placeholderTitleLabel->font();
        placeholderTitleFont.setPointSize(placeholderTitleFont.pointSize() + 2);
        placeholderTitleFont.setBold(true);
        m_placeholderTitleLabel->setFont(placeholderTitleFont);
        m_placeholderTitleLabel->setWordWrap(true);
        makeLabelSelectable(m_placeholderTitleLabel);

        m_placeholderDetailLabel = new QLabel(placeholderCard);
        m_placeholderDetailLabel->setWordWrap(true);
        m_placeholderDetailLabel->setStyleSheet(QStringLiteral("color: #c5cad3;"));
        makeLabelSelectable(m_placeholderDetailLabel);

        m_loadingIndicator = new QProgressBar(placeholderCard);
        m_loadingIndicator->setRange(0, 0);
        m_loadingIndicator->setTextVisible(false);
        m_loadingIndicator->setFixedHeight(8);
        m_loadingIndicator->setVisible(false);
        m_loadingIndicator->setStyleSheet(QStringLiteral("QProgressBar {"
                                                         " background: #2a2d34;"
                                                         " border: 1px solid #393d46;"
                                                         " border-radius: 4px;"
                                                         "}"
                                                         "QProgressBar::chunk {"
                                                         " background: #7fb0ff;"
                                                         " border-radius: 4px;"
                                                         "}"));

        placeholderCardLayout->addWidget(m_placeholderTitleLabel);
        placeholderCardLayout->addWidget(m_placeholderDetailLabel);
        placeholderCardLayout->addWidget(m_loadingIndicator);

        placeholderOuterLayout->addWidget(placeholderCard, 0, Qt::AlignHCenter);
        placeholderOuterLayout->addStretch(1);

        m_plainTextView = new QPlainTextEdit(this);
        m_plainTextView->setReadOnly(true);
        m_plainTextView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

        m_multipleSelectionScrollArea = new QScrollArea(this);
        m_multipleSelectionScrollArea->setWidgetResizable(true);
        m_multipleSelectionScrollArea->setFrameShape(QFrame::NoFrame);
        m_multipleSelectionScrollArea->setSizePolicy(QSizePolicy::Expanding,
                                                     QSizePolicy::Expanding);
        m_multipleSelectionWidget = new QWidget(m_multipleSelectionScrollArea);
        m_multipleSelectionLayout = new QVBoxLayout(m_multipleSelectionWidget);
        m_multipleSelectionLayout->setContentsMargins(0, 0, 0, 0);
        m_multipleSelectionLayout->setSpacing(0);
        m_multipleSelectionScrollArea->setWidget(m_multipleSelectionWidget);

        m_htmlView = new HtmlMessageView(this);
        m_htmlView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        connect(m_htmlView, &HtmlMessageView::viewSourceRequested, this,
                &MessageViewContainer::viewSourceRequested);

        m_bodyStack->addWidget(m_placeholderPanel);
        m_bodyStack->addWidget(m_multipleSelectionScrollArea);
        m_bodyStack->addWidget(m_plainTextView);
        m_bodyStack->addWidget(m_htmlView);

        m_attachmentStatusLabel = new QLabel(this);
        m_attachmentStatusLabel->setWordWrap(true);
        makeLabelSelectable(m_attachmentStatusLabel);
        m_attachmentStatusLabel->setVisible(false);

        m_attachmentHeaderWidget = new QWidget(this);
        auto* attachmentHeaderLayout = new QHBoxLayout(m_attachmentHeaderWidget);
        attachmentHeaderLayout->setContentsMargins(0, 0, 0, 0);
        attachmentHeaderLayout->setSpacing(4);

        m_attachmentExpanderButton = new QToolButton(m_attachmentHeaderWidget);
        m_attachmentExpanderButton->setAutoRaise(true);
        m_attachmentExpanderButton->setArrowType(Qt::RightArrow);
        connect(m_attachmentExpanderButton, &QToolButton::clicked, this,
                [this]
                {
                    m_attachmentsExpanded = !m_attachmentsExpanded;
                    updateAttachmentSection();
                });

        m_attachmentStatusLabel->setParent(m_attachmentHeaderWidget);

        m_saveAllAttachmentsButton = new QToolButton(m_attachmentHeaderWidget);
        m_saveAllAttachmentsButton->setText(QStringLiteral("Save All"));
        connect(m_saveAllAttachmentsButton, &QToolButton::clicked, this,
                [this]
                {
                    if (m_accountId.has_value() && m_emailId.has_value())
                    {
                        Q_EMIT saveAllAttachmentsRequested(QString::fromStdString(*m_accountId),
                                                           QString::fromStdString(*m_emailId));
                    }
                });

        attachmentHeaderLayout->addWidget(m_attachmentExpanderButton);
        attachmentHeaderLayout->addWidget(m_attachmentStatusLabel, 1);
        attachmentHeaderLayout->addWidget(m_saveAllAttachmentsButton);
        m_attachmentHeaderWidget->setVisible(false);

        m_attachmentListWidget = new QWidget(this);
        m_attachmentListLayout = new QGridLayout(m_attachmentListWidget);
        m_attachmentListLayout->setContentsMargins(0, 0, 0, 0);
        m_attachmentListLayout->setHorizontalSpacing(6);
        m_attachmentListLayout->setVerticalSpacing(6);
        m_attachmentListWidget->setVisible(false);

        layout->addWidget(headerWidget);
        layout->addWidget(m_bodyControlsWidget);
        layout->addWidget(m_bodyStack, 1);
        layout->addWidget(m_attachmentHeaderWidget);
        layout->addWidget(m_attachmentListWidget);

        updatePresentation();
    }

    MessageViewContainer::~MessageViewContainer() = default;

    void
    MessageViewContainer::setSelection(javelin::jmap::cache::MessageViewService& messageViewService,
                                       std::optional<std::string> accountId,
                                       std::optional<std::string> mailboxId,
                                       std::optional<std::string> emailId)
    {
        if (m_accountId == accountId && m_mailboxId == mailboxId && m_emailId == emailId)
        {
            return;
        }

        m_accountId = std::move(accountId);
        m_mailboxId = std::move(mailboxId);
        m_emailId = std::move(emailId);
        m_multipleMessages.clear();
        m_attachmentsExpanded = false;
        m_loading = false;
        m_errorMessage.clear();

        m_snapshot = std::nullopt;
        if (m_accountId.has_value() && m_emailId.has_value())
        {
            const auto result = messageViewService.load(*m_accountId, *m_emailId);
            if (const auto* snapshot =
                    std::get_if<std::optional<javelin::jmap::cache::MessageViewSnapshot>>(&result))
            {
                m_snapshot = *snapshot;
            }
        }

        updatePresentation();
    }

    void MessageViewContainer::setMultipleSelection(
        std::optional<std::string> accountId, std::optional<std::string> mailboxId,
        std::vector<javelin::jmap::cache::MessageListItem> messages)
    {
        m_accountId = std::move(accountId);
        m_mailboxId = std::move(mailboxId);
        m_emailId = std::nullopt;
        m_multipleMessages = std::move(messages);
        m_attachmentsExpanded = false;
        m_loading = false;
        m_errorMessage.clear();
        m_snapshot = std::nullopt;
        updatePresentation();
    }

    void MessageViewContainer::refresh(javelin::jmap::cache::MessageViewService& messageViewService)
    {
        const auto previousRenderedBody = renderedBodyKey(m_snapshot);
        m_loading = false;
        m_errorMessage.clear();
        m_snapshot = std::nullopt;
        if (m_accountId.has_value() && m_emailId.has_value())
        {
            const auto result = messageViewService.load(*m_accountId, *m_emailId);
            if (const auto* snapshot =
                    std::get_if<std::optional<javelin::jmap::cache::MessageViewSnapshot>>(&result))
            {
                m_snapshot = *snapshot;
            }
        }

        updatePresentation(previousRenderedBody != renderedBodyKey(m_snapshot));
    }

    void MessageViewContainer::setActiveView(const ActiveView view)
    {
        m_activeView = view;
        updateSenderRemoteContentPermit();
        updateRemoteContentButton();

        switch (m_activeView)
        {
        case ActiveView::Placeholder:
            m_bodyStack->setCurrentWidget(m_placeholderPanel);
            break;
        case ActiveView::Multiple:
            m_bodyStack->setCurrentWidget(m_multipleSelectionScrollArea);
            break;
        case ActiveView::PlainText:
            m_bodyStack->setCurrentWidget(m_plainTextView);
            break;
        case ActiveView::Html:
            m_bodyStack->setCurrentWidget(m_htmlView);
            break;
        }
    }

    void MessageViewContainer::setLoadingState(const bool loading, const QString& detailText)
    {
        m_loading = loading;
        if (loading)
        {
            m_errorMessage.clear();
        }
        if (!detailText.isEmpty())
        {
            m_placeholderDetailLabel->setText(detailText);
        }
        updatePresentation();
    }

    void MessageViewContainer::setErrorState(const QString& errorMessage)
    {
        m_loading = false;
        m_errorMessage = errorMessage;
        updatePresentation();
    }

    bool MessageViewContainer::hasContentSnapshot() const
    {
        return m_snapshot.has_value();
    }

    bool MessageViewContainer::hasReadableBody() const
    {
        return m_snapshot.has_value() &&
               (m_snapshot->htmlBody.has_value() || m_snapshot->plainTextBody.has_value());
    }

    void MessageViewContainer::updateSenderRemoteContentPermit()
    {
        const auto sender = currentSenderAddress();
        const auto domain = currentSenderDomain();
        const bool permittedSender =
            !sender.isEmpty() &&
            remoteContentAllowList(QLatin1StringView{allowedSendersKey})
                .contains(sender, Qt::CaseInsensitive);
        const bool permittedDomain =
            !domain.isEmpty() &&
            remoteContentAllowList(QLatin1StringView{allowedDomainsKey})
                .contains(domain, Qt::CaseInsensitive);
        const bool shouldAllow = permittedSender || permittedDomain;
        if (m_htmlView->remoteContentEnabled() != shouldAllow)
        {
            m_htmlView->setRemoteContentEnabled(shouldAllow);
        }
    }

    void MessageViewContainer::updateRemoteContentButton()
    {
        const bool hasBlockedRemoteContent =
            m_snapshot.has_value() && m_snapshot->htmlRenderDocument.has_value() &&
            m_snapshot->htmlRenderDocument->blockedRemoteResourceCount > 0;
        m_bodyControlsWidget->setVisible(hasBlockedRemoteContent);
        m_remoteContentStatusLabel->setVisible(hasBlockedRemoteContent);
        m_permitSenderRemoteContentButton->setVisible(hasBlockedRemoteContent);
        m_permitDomainRemoteContentButton->setVisible(hasBlockedRemoteContent);
        m_permitSenderRemoteContentButton->setEnabled(hasBlockedRemoteContent &&
                                                      !currentSenderAddress().isEmpty());
        m_permitDomainRemoteContentButton->setEnabled(hasBlockedRemoteContent &&
                                                      !currentSenderDomain().isEmpty());
        m_remoteContentButton->setVisible(hasBlockedRemoteContent);
        m_remoteContentButton->setEnabled(hasBlockedRemoteContent);
        m_remoteContentButton->setChecked(hasBlockedRemoteContent &&
                                          m_htmlView->remoteContentEnabled());
        if (hasBlockedRemoteContent)
        {
            m_remoteContentStatusLabel->setText(
                QStringLiteral("Blocked remote resources: %1")
                    .arg(static_cast<qulonglong>(
                        m_snapshot->htmlRenderDocument->blockedRemoteResourceCount)));
        }
        else
        {
            m_remoteContentStatusLabel->clear();
        }
        m_permitSenderRemoteContentButton->setToolTip(
            currentSenderAddress().isEmpty()
                ? QStringLiteral("No sender address is available")
                : QStringLiteral("Always load remote content from this sender"));
        m_permitDomainRemoteContentButton->setToolTip(
            currentSenderDomain().isEmpty()
                ? QStringLiteral("No sender domain is available")
                : QStringLiteral("Always load remote content from this sender domain"));
        m_remoteContentButton->setText(m_htmlView->remoteContentEnabled()
                                           ? QStringLiteral("Hide remote content")
                                           : QStringLiteral("Load remote content"));
    }

    void MessageViewContainer::updatePresentation(const bool reloadBody)
    {
        if (reloadBody)
        {
            m_plainTextView->clear();
            m_htmlView->clearDocument();
        }
        m_attachmentStatusLabel->clear();
        rebuildAttachmentRows();
        rebuildMultipleSelectionRows();
        updateAttachmentSection();
        updateRemoteContentButton();
        m_loadingIndicator->setVisible(false);

        if (!m_accountId.has_value())
        {
            m_titleLabel->setText(QStringLiteral("Choose an account"));
            m_detailLabel->setText(QStringLiteral("Select an account to browse your mail."));
            m_placeholderTitleLabel->setText(QStringLiteral("Ready when you are"));
            m_placeholderDetailLabel->setText(
                QStringLiteral("Message details will appear here after you choose an account, "
                               "mailbox, and message."));
            setActiveView(ActiveView::Placeholder);
            return;
        }

        if (!m_mailboxId.has_value() && !m_emailId.has_value())
        {
            m_titleLabel->setText(QStringLiteral("Choose a mailbox"));
            m_detailLabel->setText(
                QStringLiteral("Select a mailbox in the left pane to populate the message list."));
            m_placeholderTitleLabel->setText(QStringLiteral("Choose a mailbox"));
            m_placeholderDetailLabel->setText(
                QStringLiteral("Choose a mailbox to see its messages here."));
            setActiveView(ActiveView::Placeholder);
            return;
        }

        if (!m_multipleMessages.empty())
        {
            m_titleLabel->setText(QStringLiteral("%1 messages selected")
                                      .arg(static_cast<qulonglong>(m_multipleMessages.size())));
            m_detailLabel->clear();
            m_bodyControlsWidget->setVisible(false);
            setActiveView(ActiveView::Multiple);
            return;
        }

        if (!m_emailId.has_value())
        {
            m_titleLabel->setText(QStringLiteral("Choose a message"));
            m_detailLabel->setText(
                QStringLiteral("Select a message in the center pane to open it here."));
            m_placeholderTitleLabel->setText(QStringLiteral("Choose a message"));
            m_placeholderDetailLabel->setText(QStringLiteral("Select a message to read it here."));
            setActiveView(ActiveView::Placeholder);
            return;
        }

        if (!m_snapshot.has_value())
        {
            if (!m_errorMessage.isEmpty())
            {
                m_titleLabel->setText(QStringLiteral("Could not load message"));
                m_detailLabel->setText(m_errorMessage);
                m_placeholderTitleLabel->setText(QStringLiteral("Message retrieval failed"));
                m_placeholderDetailLabel->setText(m_errorMessage);
            }
            else if (m_loading)
            {
                m_titleLabel->setText(QStringLiteral("Loading message"));
                m_detailLabel->setText(QStringLiteral("Downloading the selected message now."));
                m_placeholderTitleLabel->setText(QStringLiteral("Loading message"));
                if (m_placeholderDetailLabel->text().isEmpty())
                {
                    m_placeholderDetailLabel->setText(
                        QStringLiteral("Downloading the selected message now."));
                }
                m_loadingIndicator->setVisible(true);
            }
            else
            {
                m_titleLabel->setText(QStringLiteral("Message is unavailable"));
                m_detailLabel->setText(
                    QStringLiteral("This message is not available on this device yet."));
                m_placeholderTitleLabel->setText(QStringLiteral("Message unavailable"));
                m_placeholderDetailLabel->setText(QStringLiteral(
                    "Try refreshing the mailbox or reopening the message in a moment."));
            }
            setActiveView(ActiveView::Placeholder);
            return;
        }

        const auto sender = !m_snapshot->email.from.empty()
                                ? QString::fromStdString(m_snapshot->email.from.front().email)
                                : QStringLiteral("(unknown sender)");
        const auto subject = m_snapshot->email.subject.has_value()
                                 ? QString::fromStdString(*m_snapshot->email.subject)
                                 : QStringLiteral("(no subject)");

        m_titleLabel->setText(subject);
        m_detailLabel->setText(
            QStringLiteral("From %1\nReceived %2")
                .arg(sender, QString::fromStdString(m_snapshot->email.receivedAt)));

        if (reloadBody && m_snapshot->plainTextBody.has_value())
        {
            m_plainTextView->setPlainText(QString::fromStdString(m_snapshot->plainTextBody->value));
        }

        if (reloadBody && m_snapshot->htmlBody.has_value())
        {
            const auto renderDocument =
                m_snapshot->htmlRenderDocument.has_value()
                    ? QString::fromStdString(m_snapshot->htmlRenderDocument->html)
                    : QString::fromStdString(m_snapshot->htmlBody->value);
            m_htmlView->setDocumentHtml(renderDocument.toStdString());
        }
        updateSenderRemoteContentPermit();

        m_attachmentStatusLabel->setText(attachmentStatusText());
        rebuildAttachmentRows();
        updateAttachmentSection();
        updateRemoteContentButton();

        if (m_snapshot->htmlBody.has_value())
        {
            setActiveView(ActiveView::Html);
        }
        else if (m_snapshot->plainTextBody.has_value())
        {
            setActiveView(ActiveView::PlainText);
        }
        else
        {
            if (m_loading)
            {
                m_titleLabel->setText(QStringLiteral("Loading message"));
                m_detailLabel->setText(QStringLiteral("Downloading the selected message now."));
                m_placeholderTitleLabel->setText(QStringLiteral("Loading message"));
                m_placeholderDetailLabel->setText(
                    QStringLiteral("Downloading the selected message now."));
                m_loadingIndicator->setVisible(true);
            }
            else
            {
                m_placeholderTitleLabel->setText(QStringLiteral("Nothing to display"));
                m_placeholderDetailLabel->setText(QStringLiteral(
                    "This message does not currently have a readable body available."));
            }
            setActiveView(ActiveView::Placeholder);
        }
    }

    void MessageViewContainer::permitRemoteContentForCurrentSender()
    {
        addRemoteContentAllowListValue(QLatin1StringView{allowedSendersKey},
                                       currentSenderAddress());
        updateSenderRemoteContentPermit();
        updateRemoteContentButton();
    }

    void MessageViewContainer::permitRemoteContentForCurrentDomain()
    {
        addRemoteContentAllowListValue(QLatin1StringView{allowedDomainsKey},
                                       currentSenderDomain());
        updateSenderRemoteContentPermit();
        updateRemoteContentButton();
    }

    QString MessageViewContainer::currentSenderAddress() const
    {
        if (!m_snapshot.has_value() || m_snapshot->email.from.empty())
        {
            return {};
        }
        return QString::fromStdString(m_snapshot->email.from.front().email).trimmed().toLower();
    }

    QString MessageViewContainer::currentSenderDomain() const
    {
        const auto sender = currentSenderAddress();
        const auto atIndex = sender.lastIndexOf(QLatin1Char('@'));
        if (atIndex < 0 || atIndex + 1 >= sender.size())
        {
            return {};
        }
        return sender.sliced(atIndex + 1).trimmed().toLower();
    }

    void MessageViewContainer::rebuildAttachmentRows()
    {
        while (QLayoutItem* item = m_attachmentListLayout->takeAt(0))
        {
            if (QWidget* widget = item->widget())
            {
                widget->deleteLater();
            }
            delete item;
        }

        const auto attachments = visibleAttachments(m_snapshot);
        const bool hasAttachments = !attachments.empty();
        m_attachmentListWidget->setVisible(hasAttachments && m_attachmentsExpanded);
        if (!hasAttachments || !m_accountId.has_value() || !m_emailId.has_value())
        {
            return;
        }

        constexpr int targetTileWidth = 220;
        constexpr int tileSpacing = 6;
        const int availableWidth = std::max(width(), m_attachmentListWidget->width());
        const int columnCount =
            std::max(1, (availableWidth + tileSpacing) / (targetTileWidth + tileSpacing));

        for (std::size_t index = 0; index < attachments.size(); ++index)
        {
            const auto* attachment = attachments.at(index);
            auto* tile = new AttachmentTile(
                *attachment,
                [this, partId = QString::fromStdString(attachment->partId)]
                {
                    Q_EMIT openAttachmentRequested(QString::fromStdString(*m_accountId),
                                                   QString::fromStdString(*m_emailId), partId);
                },
                [this, partId = QString::fromStdString(attachment->partId)]
                {
                    Q_EMIT saveAttachmentRequested(QString::fromStdString(*m_accountId),
                                                   QString::fromStdString(*m_emailId), partId);
                },
                m_attachmentListWidget);
            tile->setMinimumWidth(targetTileWidth);

            const int row = static_cast<int>(index) / columnCount;
            const int column = static_cast<int>(index) % columnCount;
            m_attachmentListLayout->addWidget(tile, row, column);
        }

        for (int column = 0; column < columnCount; ++column)
        {
            m_attachmentListLayout->setColumnStretch(column, 1);
        }
    }

    void MessageViewContainer::rebuildMultipleSelectionRows()
    {
        while (QLayoutItem* item = m_multipleSelectionLayout->takeAt(0))
        {
            if (QWidget* widget = item->widget())
            {
                widget->deleteLater();
            }
            delete item;
        }

        for (std::size_t index = 0; index < m_multipleMessages.size(); ++index)
        {
            const auto& message = m_multipleMessages[index];
            auto* tile = new MessagePreviewTile(
                message, [this, emailId = QString::fromStdString(message.emailId)]
                { Q_EMIT messageActivated(emailId); }, m_multipleSelectionWidget);
            m_multipleSelectionLayout->addWidget(tile);

            if (index + 1 < m_multipleMessages.size())
            {
                auto* separator = new QFrame(m_multipleSelectionWidget);
                separator->setFrameShape(QFrame::HLine);
                separator->setFrameShadow(QFrame::Plain);
                separator->setStyleSheet(QStringLiteral("color: #393d46;"));
                m_multipleSelectionLayout->addWidget(separator);
            }
        }

        m_multipleSelectionLayout->addStretch(1);
    }

    QString MessageViewContainer::attachmentStatusText() const
    {
        if (!m_snapshot.has_value())
        {
            return {};
        }

        const auto attachments = visibleAttachments(m_snapshot);
        if (!attachments.empty())
        {
            std::uint64_t totalSize = 0;
            for (const auto* attachment : attachments)
            {
                totalSize += attachment->size;
            }

            const auto attachmentCount = static_cast<qulonglong>(attachments.size());
            return QStringLiteral("%1 %2  %3")
                .arg(attachmentCount)
                .arg(attachmentCount == 1 ? QStringLiteral("attachment")
                                          : QStringLiteral("attachments"))
                .arg(QLocale{}.formattedDataSize(static_cast<qint64>(totalSize)));
        }

        if (m_snapshot->htmlRenderDocument.has_value() &&
            m_snapshot->htmlRenderDocument->inlineResourceCount > 0)
        {
            return QStringLiteral("Inline resources: %1")
                .arg(static_cast<qulonglong>(m_snapshot->htmlRenderDocument->inlineResourceCount));
        }

        return {};
    }

    void MessageViewContainer::updateAttachmentSection()
    {
        const bool hasAttachments = !visibleAttachments(m_snapshot).empty();
        m_attachmentHeaderWidget->setVisible(hasAttachments);
        m_attachmentStatusLabel->setVisible(hasAttachments);
        m_attachmentExpanderButton->setVisible(hasAttachments);
        m_saveAllAttachmentsButton->setVisible(hasAttachments);
        m_saveAllAttachmentsButton->setEnabled(hasAttachments);
        m_attachmentExpanderButton->setArrowType(m_attachmentsExpanded ? Qt::DownArrow
                                                                       : Qt::RightArrow);
        m_attachmentListWidget->setVisible(hasAttachments && m_attachmentsExpanded);
    }

    void MessageViewContainer::resizeEvent(QResizeEvent* event)
    {
        QWidget::resizeEvent(event);
        if (m_attachmentsExpanded && !visibleAttachments(m_snapshot).empty())
        {
            rebuildAttachmentRows();
        }
    }

} // namespace javelin::gui::messageview
