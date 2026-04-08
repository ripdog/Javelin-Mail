#include "gui/messageview/MessageViewContainer.h"
#include "gui/messageview/HtmlMessageView.h"

#include <QApplication>
#include <QFrame>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStyle>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <vector>

namespace javelin::gui::messageview
{
    namespace
    {

        [[nodiscard]] QString attachmentName(const javelin::jmap::cache::MessageAttachment& attachment)
        {
            return QString::fromStdString(attachment.name.value_or(attachment.partId));
        }

        [[nodiscard]] QString attachmentSizeLabel(
            const javelin::jmap::cache::MessageAttachment& attachment)
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

        [[nodiscard]] QIcon attachmentIcon(const javelin::jmap::cache::MessageAttachment& attachment)
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
        placeholderCard->setStyleSheet(QStringLiteral(
            "#messagePlaceholderCard {"
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
        m_loadingIndicator->setStyleSheet(QStringLiteral(
            "QProgressBar {"
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

        m_htmlView = new HtmlMessageView(this);
        m_htmlView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        connect(m_htmlView, &HtmlMessageView::viewSourceRequested, this,
                &MessageViewContainer::viewSourceRequested);

        m_bodyStack->addWidget(m_placeholderPanel);
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
        m_attachmentsExpanded = false;
        m_loading = false;

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

    void MessageViewContainer::refresh(javelin::jmap::cache::MessageViewService& messageViewService)
    {
        m_loading = false;
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

    void MessageViewContainer::setActiveView(const ActiveView view)
    {
        m_activeView = view;
        updateRemoteContentButton();

        switch (m_activeView)
        {
        case ActiveView::Placeholder:
            m_bodyStack->setCurrentWidget(m_placeholderPanel);
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
        if (!detailText.isEmpty())
        {
            m_placeholderDetailLabel->setText(detailText);
        }
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

    void MessageViewContainer::updateRemoteContentButton()
    {
        const bool hasBlockedRemoteContent =
            m_snapshot.has_value() && m_snapshot->htmlRenderDocument.has_value() &&
            m_snapshot->htmlRenderDocument->blockedRemoteResourceCount > 0;
        m_bodyControlsWidget->setVisible(hasBlockedRemoteContent);
        m_remoteContentStatusLabel->setVisible(hasBlockedRemoteContent);
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
        m_remoteContentButton->setText(m_htmlView->remoteContentEnabled()
                                           ? QStringLiteral("Hide remote content")
                                           : QStringLiteral("Load remote content"));
    }

    void MessageViewContainer::updatePresentation()
    {
        m_plainTextView->clear();
        m_htmlView->clearDocument();
        m_attachmentStatusLabel->clear();
        rebuildAttachmentRows();
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

        if (!m_mailboxId.has_value())
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

        if (!m_emailId.has_value())
        {
            m_titleLabel->setText(QStringLiteral("Choose a message"));
            m_detailLabel->setText(
                QStringLiteral("Select a message in the center pane to open it here."));
            m_placeholderTitleLabel->setText(QStringLiteral("Choose a message"));
            m_placeholderDetailLabel->setText(
                QStringLiteral("Select a message to read it here."));
            setActiveView(ActiveView::Placeholder);
            return;
        }

        if (!m_snapshot.has_value())
        {
            if (m_loading)
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

        if (m_snapshot->plainTextBody.has_value())
        {
            m_plainTextView->setPlainText(QString::fromStdString(m_snapshot->plainTextBody->value));
        }

        if (m_snapshot->htmlBody.has_value())
        {
            const auto renderDocument =
                m_snapshot->htmlRenderDocument.has_value()
                    ? QString::fromStdString(m_snapshot->htmlRenderDocument->html)
                    : QString::fromStdString(m_snapshot->htmlBody->value);
            m_htmlView->setDocumentHtml(renderDocument.toStdString());
        }

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
        const int columnCount = std::max(1, (availableWidth + tileSpacing) /
                                                (targetTileWidth + tileSpacing));

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
                .arg(static_cast<qulonglong>(
                    m_snapshot->htmlRenderDocument->inlineResourceCount));
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
