#include "gui/messageview/MessageViewContainer.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace javelin::gui::messageview
{

    MessageViewContainer::MessageViewContainer(QWidget* parent) : QWidget(parent)
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(12);

        m_titleLabel = new QLabel(this);
        m_titleLabel->setObjectName(QStringLiteral("messageViewTitle"));

        auto titleFont = m_titleLabel->font();
        titleFont.setPointSize(titleFont.pointSize() + 4);
        titleFont.setBold(true);
        m_titleLabel->setFont(titleFont);

        m_detailLabel = new QLabel(this);
        m_detailLabel->setWordWrap(true);

        auto* buttonRow = new QWidget(this);
        auto* buttonLayout = new QHBoxLayout(buttonRow);
        buttonLayout->setContentsMargins(0, 0, 0, 0);
        buttonLayout->setSpacing(8);

        m_plainTextButton = new QToolButton(buttonRow);
        m_plainTextButton->setText(QStringLiteral("Plain text"));
        m_plainTextButton->setCheckable(true);
        connect(m_plainTextButton, &QToolButton::clicked, this,
                [this] { setActiveView(ActiveView::PlainText); });

        m_htmlButton = new QToolButton(buttonRow);
        m_htmlButton->setText(QStringLiteral("HTML source"));
        m_htmlButton->setCheckable(true);
        connect(m_htmlButton, &QToolButton::clicked, this,
                [this] { setActiveView(ActiveView::Html); });

        buttonLayout->addWidget(m_plainTextButton);
        buttonLayout->addWidget(m_htmlButton);
        buttonLayout->addStretch(1);

        m_bodyStack = new QStackedWidget(this);

        m_placeholderLabel = new QLabel(this);
        m_placeholderLabel->setWordWrap(true);

        m_plainTextView = new QPlainTextEdit(this);
        m_plainTextView->setReadOnly(true);

        m_htmlView = new QPlainTextEdit(this);
        m_htmlView->setReadOnly(true);

        m_bodyStack->addWidget(m_placeholderLabel);
        m_bodyStack->addWidget(m_plainTextView);
        m_bodyStack->addWidget(m_htmlView);

        m_attachmentLabel = new QLabel(this);
        m_attachmentLabel->setWordWrap(true);

        layout->addWidget(m_titleLabel);
        layout->addWidget(m_detailLabel);
        layout->addWidget(buttonRow);
        layout->addWidget(m_bodyStack, 1);
        layout->addWidget(m_attachmentLabel);
        layout->addStretch(1);

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
        updateBodyButtons();

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

    void MessageViewContainer::updateBodyButtons()
    {
        const bool hasPlainText = m_snapshot.has_value() && m_snapshot->plainTextBody.has_value();
        const bool hasHtml = m_snapshot.has_value() && m_snapshot->htmlBody.has_value();
        m_plainTextButton->setEnabled(hasPlainText);
        m_htmlButton->setEnabled(hasHtml);
        m_plainTextButton->setChecked(m_activeView == ActiveView::PlainText);
        m_htmlButton->setChecked(m_activeView == ActiveView::Html);
    }

    void MessageViewContainer::updatePresentation()
    {
        m_plainTextView->clear();
        m_htmlView->clear();
        m_attachmentLabel->clear();

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
            m_htmlView->setPlainText(QString::fromStdString(m_snapshot->htmlBody->value));
        }

        if (!m_snapshot->attachments.empty())
        {
            QStringList attachmentNames;
            attachmentNames.reserve(static_cast<qsizetype>(m_snapshot->attachments.size()));
            for (const auto& attachment : m_snapshot->attachments)
            {
                attachmentNames.push_back(
                    QString::fromStdString(attachment.name.value_or(attachment.partId)));
            }

            m_attachmentLabel->setText(
                QStringLiteral("Attachments: %1").arg(attachmentNames.join(QStringLiteral(", "))));
        }

        if (m_snapshot->plainTextBody.has_value())
        {
            setActiveView(ActiveView::PlainText);
        }
        else if (m_snapshot->htmlBody.has_value())
        {
            setActiveView(ActiveView::Html);
        }
        else
        {
            m_placeholderLabel->setText(QStringLiteral(
                "No cached plain-text or HTML body is available for this message yet."));
            setActiveView(ActiveView::Placeholder);
        }
    }

} // namespace javelin::gui::messageview
