#include "gui/shell/QuickFilterController.h"

#include "app/MailboxSession.h"
#include "gui/IconUtils.h"
#include "jmap/cache/MailTagReader.h"

#include <KLocalizedString>

#include <QAction>
#include <QColor>
#include <QLineEdit>
#include <QMenu>
#include <QPixmap>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QWidget>

#include <algorithm>

namespace javelin::gui::shell
{
    namespace
    {
        [[nodiscard]] QIcon tagColorIcon(const QStringView colorName)
        {
            const QColor color{colorName.toString()};
            if (!color.isValid())
                return {};
            QPixmap swatch{14, 14};
            swatch.fill(color);
            return QIcon{swatch};
        }
    } // namespace

    QuickFilterController::QuickFilterController(javelin::jmap::cache::MailTagReader& mailTagReader,
                                                 QuickFilterWidgets widgets,
                                                 QWidget& shortcutParent, QObject* parent)
        : QObject(parent), m_mailTagReader(mailTagReader), m_widgets(widgets)
    {
        connect(&m_widgets.toggleButton, &QToolButton::toggled, this,
                [this](const bool visible)
                {
                    m_widgets.panel.setVisible(visible && activeMailbox() != nullptr);
                    if (visible)
                        rebuildTagsMenu();
                });

        auto* showShortcut =
            new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K), &shortcutParent);
        connect(showShortcut, &QShortcut::activated, this,
                [this]
                {
                    if (activeMailbox() == nullptr)
                        return;
                    m_widgets.toggleButton.setChecked(true);
                    m_widgets.textEdit.setFocus(Qt::ShortcutFocusReason);
                    m_widgets.textEdit.selectAll();
                });
        auto* hideShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), &shortcutParent);
        connect(hideShortcut, &QShortcut::activated, this,
                [this]
                {
                    if (m_widgets.panel.isVisible())
                        m_widgets.toggleButton.setChecked(false);
                });

        auto* textTimer = new QTimer(this);
        textTimer->setSingleShot(true);
        textTimer->setInterval(250);
        connect(textTimer, &QTimer::timeout, this, &QuickFilterController::apply);
        connect(&m_widgets.textEdit, &QLineEdit::textChanged, this,
                [textTimer] { textTimer->start(); });

        for (auto* button :
             {&m_widgets.unreadButton, &m_widgets.starredButton, &m_widgets.contactButton,
              &m_widgets.tagsButton, &m_widgets.attachmentButton, &m_widgets.senderButton,
              &m_widgets.recipientsButton, &m_widgets.subjectButton, &m_widgets.bodyButton})
        {
            connect(button, &QToolButton::toggled, this, [this] { apply(); });
        }
        connect(&m_widgets.pinButton, &QToolButton::toggled, this,
                [this](const bool pinned)
                {
                    m_pinned = pinned;
                    if (pinned)
                        m_pinnedCriteria = criteriaFromUi();
                });
        connect(&m_widgets.tagsMenu, &QMenu::aboutToShow, this,
                &QuickFilterController::rebuildTagsMenu);
    }

    MailboxTabState* QuickFilterController::activeMailbox() const
    {
        return m_activeTab == nullptr ? nullptr
                                      : std::get_if<MailboxTabState>(&m_activeTab->content);
    }

    javelin::jmap::search::EmailSearchCriteria QuickFilterController::criteriaFromUi() const
    {
        javelin::jmap::search::EmailSearchCriteria criteria;
        criteria.unreadOnly = m_widgets.unreadButton.isChecked();
        criteria.starredOnly = m_widgets.starredButton.isChecked();
        criteria.fromContactsOnly = m_widgets.contactButton.isChecked();
        criteria.taggedOnly = m_widgets.tagsButton.isChecked();
        criteria.hasAttachmentOnly = m_widgets.attachmentButton.isChecked();
        if (criteria.taggedOnly)
            criteria.tags = m_tags;
        criteria.matchAllTags = m_matchAllTags;
        const auto text = m_widgets.textEdit.text().trimmed();
        if (!text.isEmpty())
            criteria.quickText = text.toStdString();
        criteria.quickTextSender = m_widgets.senderButton.isChecked();
        criteria.quickTextRecipients = m_widgets.recipientsButton.isChecked();
        criteria.quickTextSubject = m_widgets.subjectButton.isChecked();
        criteria.quickTextBody = m_widgets.bodyButton.isChecked();
        return criteria;
    }

    void QuickFilterController::apply()
    {
        auto* mailbox = activeMailbox();
        if (mailbox == nullptr || mailbox->session == nullptr)
            return;

        auto criteria = criteriaFromUi();
        if (m_pinned)
            m_pinnedCriteria = criteria;
        mailbox->session->setQuickFilter(std::move(criteria));
    }

    void QuickFilterController::activate(TabState* tab)
    {
        m_activeTab = tab;
        auto* mailbox = activeMailbox();
        if (mailbox != nullptr && mailbox->session != nullptr)
        {
            const auto identity =
                std::pair{mailbox->session->accountId(), mailbox->session->mailboxId()};
            if (m_lastMailbox.has_value() && *m_lastMailbox != identity)
            {
                mailbox->session->setQuickFilter(
                    m_pinned ? m_pinnedCriteria : javelin::jmap::search::EmailSearchCriteria{});
            }
            else if (m_pinned)
            {
                mailbox->session->setQuickFilter(m_pinnedCriteria);
            }
            m_lastMailbox = identity;
        }
        syncUiFromSession();
    }

    void QuickFilterController::syncContinuitySelection(std::optional<std::string> emailId,
                                                        std::optional<std::string> threadId)
    {
        auto* mailbox = activeMailbox();
        if (mailbox == nullptr || mailbox->session == nullptr)
            return;
        mailbox->session->setQuickFilterContinuitySelection(std::move(emailId),
                                                            std::move(threadId));
    }

    void QuickFilterController::syncUiFromSession()
    {
        auto* mailbox = activeMailbox();
        const bool available = mailbox != nullptr && mailbox->session != nullptr;
        m_widgets.toggleButton.setVisible(available);
        m_widgets.panel.setVisible(available && m_widgets.toggleButton.isChecked());
        if (!available)
            return;

        const auto& criteria = mailbox->session->quickFilter();
        const QSignalBlocker unreadBlocker{&m_widgets.unreadButton};
        const QSignalBlocker starredBlocker{&m_widgets.starredButton};
        const QSignalBlocker contactBlocker{&m_widgets.contactButton};
        const QSignalBlocker tagsBlocker{&m_widgets.tagsButton};
        const QSignalBlocker attachmentBlocker{&m_widgets.attachmentButton};
        const QSignalBlocker textBlocker{&m_widgets.textEdit};
        const QSignalBlocker senderBlocker{&m_widgets.senderButton};
        const QSignalBlocker recipientsBlocker{&m_widgets.recipientsButton};
        const QSignalBlocker subjectBlocker{&m_widgets.subjectButton};
        const QSignalBlocker bodyBlocker{&m_widgets.bodyButton};
        const QSignalBlocker pinBlocker{&m_widgets.pinButton};

        m_widgets.unreadButton.setChecked(criteria.unreadOnly);
        m_widgets.starredButton.setChecked(criteria.starredOnly);
        m_widgets.contactButton.setChecked(criteria.fromContactsOnly);
        m_widgets.tagsButton.setChecked(criteria.taggedOnly || !criteria.tags.empty());
        m_widgets.attachmentButton.setChecked(criteria.hasAttachmentOnly);
        m_widgets.textEdit.setText(criteria.quickText.has_value()
                                       ? QString::fromStdString(*criteria.quickText)
                                       : QString{});
        m_widgets.senderButton.setChecked(criteria.quickTextSender);
        m_widgets.recipientsButton.setChecked(criteria.quickTextRecipients);
        m_widgets.subjectButton.setChecked(criteria.quickTextSubject);
        m_widgets.bodyButton.setChecked(criteria.quickTextBody);
        m_widgets.pinButton.setChecked(m_pinned);
        m_tags = criteria.tags;
        m_matchAllTags = criteria.matchAllTags;
    }

    void QuickFilterController::rebuildTagsMenu()
    {
        m_widgets.tagsMenu.clear();

        auto* anyAction = m_widgets.tagsMenu.addAction(i18n("Any selected tag"));
        anyAction->setCheckable(true);
        anyAction->setChecked(!m_matchAllTags);
        connect(anyAction, &QAction::triggered, this,
                [this]
                {
                    m_matchAllTags = false;
                    apply();
                    rebuildTagsMenu();
                });
        auto* allAction = m_widgets.tagsMenu.addAction(i18n("All selected tags"));
        allAction->setCheckable(true);
        allAction->setChecked(m_matchAllTags);
        connect(allAction, &QAction::triggered, this,
                [this]
                {
                    m_matchAllTags = true;
                    apply();
                    rebuildTagsMenu();
                });
        m_widgets.tagsMenu.addSeparator();

        const auto* mailbox = activeMailbox();
        if (mailbox == nullptr || mailbox->session == nullptr)
            return;
        const auto result = m_mailTagReader.listTagDefinitions(mailbox->session->accountId());
        const auto* tags = std::get_if<std::vector<javelin::jmap::cache::TagDefinition>>(&result);
        if (tags == nullptr)
        {
            auto* errorAction = m_widgets.tagsMenu.addAction(i18n("Unable to load tags"));
            errorAction->setEnabled(false);
            return;
        }
        if (tags->empty())
        {
            auto* emptyAction = m_widgets.tagsMenu.addAction(i18n("No tags for this account"));
            emptyAction->setEnabled(false);
            return;
        }

        for (const auto& tag : *tags)
        {
            auto* action = m_widgets.tagsMenu.addAction(tagColorIcon(tag.color), tag.displayName);
            action->setCheckable(true);
            action->setChecked(std::ranges::find(m_tags, tag.keyword) != m_tags.end());
            connect(action, &QAction::toggled, this,
                    [this, keyword = tag.keyword](const bool checked)
                    {
                        const auto found = std::ranges::find(m_tags, keyword);
                        if (checked && found == m_tags.end())
                            m_tags.push_back(keyword);
                        else if (!checked && found != m_tags.end())
                            m_tags.erase(found);

                        const QSignalBlocker blocker{&m_widgets.tagsButton};
                        m_widgets.tagsButton.setChecked(true);
                        apply();
                    });
        }
    }

    void QuickFilterController::removeTag(const std::string_view keyword)
    {
        if (const auto found = std::ranges::find(m_tags, keyword); found != m_tags.end())
        {
            m_tags.erase(found);
            apply();
        }
        rebuildTagsMenu();
    }

    void QuickFilterController::updateIcons(const QColor& iconColor)
    {
        const auto icon = [&iconColor](const QString& resourcePath)
        { return javelin::gui::themedSvgIcon(resourcePath, iconColor); };
        m_widgets.toggleButton.setIcon(
            icon(QStringLiteral(":/icons/thunderbird-icons/filter.svg")));
        m_widgets.pinButton.setIcon(icon(QStringLiteral(":/icons/thunderbird-icons/pin.svg")));
        m_widgets.unreadButton.setIcon(
            icon(QStringLiteral(":/icons/thunderbird-icons/unread.svg")));
        m_widgets.starredButton.setIcon(icon(QStringLiteral(":/icons/thunderbird-icons/star.svg")));
        m_widgets.contactButton.setIcon(
            icon(QStringLiteral(":/icons/thunderbird-icons/address-book.svg")));
        m_widgets.tagsButton.setIcon(icon(QStringLiteral(":/icons/thunderbird-icons/tag.svg")));
        m_widgets.attachmentButton.setIcon(
            icon(QStringLiteral(":/icons/thunderbird-icons/attachment.svg")));
    }
} // namespace javelin::gui::shell
