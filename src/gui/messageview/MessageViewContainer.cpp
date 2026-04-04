#include "gui/messageview/MessageViewContainer.h"

#include <QLabel>
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

        m_bodyLabel = new QLabel(this);
        m_bodyLabel->setWordWrap(true);

        layout->addWidget(m_titleLabel);
        layout->addWidget(m_detailLabel);
        layout->addWidget(m_bodyLabel);
        layout->addStretch(1);

        updatePresentation();
    }

    MessageViewContainer::~MessageViewContainer() = default;

    void MessageViewContainer::setSelection(std::optional<std::string> accountId,
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
        updatePresentation();
    }

    void MessageViewContainer::updatePresentation()
    {
        if (!m_accountId.has_value())
        {
            m_titleLabel->setText(QStringLiteral("Choose an account"));
            m_detailLabel->setText(
                QStringLiteral("Select one of the cached JMAP accounts to browse mail."));
            m_bodyLabel->setText(
                QStringLiteral("The message viewer stays lightweight until an account, mailbox, "
                               "and message are selected."));
            return;
        }

        if (!m_mailboxId.has_value())
        {
            m_titleLabel->setText(QStringLiteral("Choose a mailbox"));
            m_detailLabel->setText(
                QStringLiteral("Select a mailbox in the left pane to populate the message list."));
            m_bodyLabel->setText(QStringLiteral("The selected account is ready. Message rendering "
                                                "will attach here after mailbox selection."));
            return;
        }

        if (!m_emailId.has_value())
        {
            m_titleLabel->setText(QStringLiteral("Choose a message"));
            m_detailLabel->setText(
                QStringLiteral("Select a message in the center pane to open it here."));
            m_bodyLabel->setText(QStringLiteral("Stage 6 will replace this placeholder with the "
                                                "plain-text and HTML viewing pipeline."));
            return;
        }

        m_titleLabel->setText(QStringLiteral("Message selected"));
        m_detailLabel->setText(
            QStringLiteral("Account `%1`, mailbox `%2`")
                .arg(QString::fromStdString(*m_accountId), QString::fromStdString(*m_mailboxId)));
        m_bodyLabel->setText(
            QStringLiteral("Selected email id: `%1`\n\nMessage rendering is not wired yet, but "
                           "the shell now tracks the active message explicitly.")
                .arg(QString::fromStdString(*m_emailId)));
    }

} // namespace javelin::gui::messageview
