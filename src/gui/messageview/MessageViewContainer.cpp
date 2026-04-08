#include "gui/messageview/MessageViewContainer.h"
#include "gui/messageview/HtmlMessageView.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QStackedWidget>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>

namespace javelin::gui::messageview
{
    namespace
    {

        [[nodiscard]] QString
        attachmentDescription(const javelin::jmap::cache::MessageAttachment& attachment)
        {
            const auto name = QString::fromStdString(attachment.name.value_or(attachment.partId));
            const auto type = QString::fromStdString(attachment.mediaType);
            return QStringLiteral("%1  •  %2  •  %3")
                .arg(name, type, QLocale{}.formattedDataSize(static_cast<qint64>(attachment.size)));
        }

    } // namespace

    MessageViewContainer::MessageViewContainer(QWidget* parent) : QWidget(parent)
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(12);

        auto* titleRow = new QWidget(this);
        auto* titleRowLayout = new QHBoxLayout(titleRow);
        titleRowLayout->setContentsMargins(0, 0, 0, 0);
        titleRowLayout->setSpacing(8);

        m_titleLabel = new QLabel(this);
        m_titleLabel->setObjectName(QStringLiteral("messageViewTitle"));

        auto titleFont = m_titleLabel->font();
        titleFont.setPointSize(titleFont.pointSize() + 4);
        titleFont.setBold(true);
        m_titleLabel->setFont(titleFont);

        titleRowLayout->addWidget(m_titleLabel, 1);

        m_detailLabel = new QLabel(this);
        m_detailLabel->setWordWrap(true);

        m_bodyControlsWidget = new QWidget(this);
        auto* buttonLayout = new QHBoxLayout(m_bodyControlsWidget);
        buttonLayout->setContentsMargins(0, 0, 0, 0);
        buttonLayout->setSpacing(8);

        m_remoteContentButton = new QToolButton(m_bodyControlsWidget);
        m_remoteContentButton->setText(QStringLiteral("Load remote content"));
        m_remoteContentButton->setCheckable(true);
        connect(m_remoteContentButton, &QToolButton::clicked, this,
                [this](const bool checked)
                {
                    m_htmlView->setRemoteContentEnabled(checked);
                    updateRemoteContentButton();
                });

        buttonLayout->addWidget(m_remoteContentButton);
        buttonLayout->addStretch(1);

        m_bodyStack = new QStackedWidget(this);

        m_placeholderLabel = new QLabel(this);
        m_placeholderLabel->setWordWrap(true);

        m_plainTextView = new QPlainTextEdit(this);
        m_plainTextView->setReadOnly(true);

        m_htmlView = new HtmlMessageView(this);

        m_bodyStack->addWidget(m_placeholderLabel);
        m_bodyStack->addWidget(m_plainTextView);
        m_bodyStack->addWidget(m_htmlView);

        m_attachmentStatusLabel = new QLabel(this);
        m_attachmentStatusLabel->setWordWrap(true);

        m_attachmentListWidget = new QWidget(this);
        m_attachmentListLayout = new QVBoxLayout(m_attachmentListWidget);
        m_attachmentListLayout->setContentsMargins(0, 0, 0, 0);
        m_attachmentListLayout->setSpacing(8);

        layout->addWidget(titleRow);
        layout->addWidget(m_detailLabel);
        layout->addWidget(m_bodyControlsWidget);
        layout->addWidget(m_bodyStack, 1);
        layout->addWidget(m_attachmentStatusLabel);
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
            m_bodyStack->setCurrentWidget(m_placeholderLabel);
            break;
        case ActiveView::PlainText:
            m_bodyStack->setCurrentWidget(m_plainTextView);
            break;
        case ActiveView::Html:
            m_bodyStack->setCurrentWidget(m_htmlView);
            break;
        }
    }

    void MessageViewContainer::updateRemoteContentButton()
    {
        const bool hasBlockedRemoteContent =
            m_snapshot.has_value() && m_snapshot->htmlRenderDocument.has_value() &&
            m_snapshot->htmlRenderDocument->blockedRemoteResourceCount > 0;
        m_bodyControlsWidget->setVisible(hasBlockedRemoteContent);
        m_remoteContentButton->setVisible(hasBlockedRemoteContent);
        m_remoteContentButton->setEnabled(hasBlockedRemoteContent);
        m_remoteContentButton->setChecked(hasBlockedRemoteContent &&
                                          m_htmlView->remoteContentEnabled());
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
        updateRemoteContentButton();

        if (!m_accountId.has_value())
        {
            m_titleLabel->setText(QStringLiteral("Choose an account"));
            m_detailLabel->setText(
                QStringLiteral("Select one of the cached JMAP accounts to browse mail."));
            m_placeholderLabel->setText(
                QStringLiteral("The message viewer stays lightweight until an account, mailbox, "
                               "and message are selected."));
            setActiveView(ActiveView::Placeholder);
            return;
        }

        if (!m_mailboxId.has_value())
        {
            m_titleLabel->setText(QStringLiteral("Choose a mailbox"));
            m_detailLabel->setText(
                QStringLiteral("Select a mailbox in the left pane to populate the message list."));
            m_placeholderLabel->setText(QStringLiteral("The selected account is ready. Message "
                                                       "rendering will attach here after mailbox "
                                                       "selection."));
            setActiveView(ActiveView::Placeholder);
            return;
        }

        if (!m_emailId.has_value())
        {
            m_titleLabel->setText(QStringLiteral("Choose a message"));
            m_detailLabel->setText(
                QStringLiteral("Select a message in the center pane to open it here."));
            m_placeholderLabel->setText(QStringLiteral(
                "Stage 6 is now wiring cache-backed message loading into this pane."));
            setActiveView(ActiveView::Placeholder);
            return;
        }

        if (!m_snapshot.has_value())
        {
            m_titleLabel->setText(QStringLiteral("Message is unavailable"));
            m_detailLabel->setText(
                QStringLiteral("The selected message is not present in the local cache yet."));
            m_placeholderLabel->setText(QStringLiteral(
                "A later sync pass or on-demand fetch path will need to populate it."));
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
            m_placeholderLabel->setText(QStringLiteral(
                "No cached plain-text or HTML body is available for this message yet."));
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

        const bool hasAttachments = m_snapshot.has_value() && !m_snapshot->attachments.empty();
        m_attachmentListWidget->setVisible(hasAttachments);
        if (!hasAttachments || !m_accountId.has_value() || !m_emailId.has_value())
        {
            return;
        }

        for (const auto& attachment : m_snapshot->attachments)
        {
            auto* row = new QWidget(m_attachmentListWidget);
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(8);

            auto* label = new QLabel(attachmentDescription(attachment), row);
            label->setWordWrap(true);

            auto* saveButton = new QToolButton(row);
            saveButton->setText(QStringLiteral("Save"));
            saveButton->setEnabled(attachment.blobId.has_value());
            connect(saveButton, &QToolButton::clicked, this,
                    [this, partId = QString::fromStdString(attachment.partId)]
                    {
                        Q_EMIT saveAttachmentRequested(QString::fromStdString(*m_accountId),
                                                       QString::fromStdString(*m_emailId), partId);
                    });

            auto* openButton = new QToolButton(row);
            openButton->setText(QStringLiteral("Open"));
            openButton->setEnabled(attachment.blobId.has_value());
            connect(openButton, &QToolButton::clicked, this,
                    [this, partId = QString::fromStdString(attachment.partId)]
                    {
                        Q_EMIT openAttachmentRequested(QString::fromStdString(*m_accountId),
                                                       QString::fromStdString(*m_emailId), partId);
                    });

            rowLayout->addWidget(label, 1);
            rowLayout->addWidget(saveButton);
            rowLayout->addWidget(openButton);
            m_attachmentListLayout->addWidget(row);
        }
    }

    QString MessageViewContainer::attachmentStatusText() const
    {
        if (!m_snapshot.has_value())
        {
            return {};
        }

        QStringList segments;
        if (!m_snapshot->attachments.empty())
        {
            segments.push_back(QStringLiteral("Attachments: %1")
                                   .arg(static_cast<qulonglong>(m_snapshot->attachments.size())));
        }

        if (m_snapshot->htmlRenderDocument.has_value())
        {
            const auto& renderDocument = *m_snapshot->htmlRenderDocument;
            if (renderDocument.inlineResourceCount > 0)
            {
                segments.push_back(
                    QStringLiteral("Inline resources: %1")
                        .arg(static_cast<qulonglong>(renderDocument.inlineResourceCount)));
            }
            if (renderDocument.blockedRemoteResourceCount > 0)
            {
                segments.push_back(
                    QStringLiteral("Blocked remote resources: %1")
                        .arg(static_cast<qulonglong>(renderDocument.blockedRemoteResourceCount)));
            }
        }

        return segments.join(QStringLiteral("\n"));
    }

} // namespace javelin::gui::messageview
