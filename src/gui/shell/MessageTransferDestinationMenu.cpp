#include "gui/shell/MessageTransferDestinationMenu.h"

#include "gui/mailboxes/MailboxPresentation.h"

#include <KLocalizedString>

#include <QAction>
#include <QEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPalette>
#include <QPointer>
#include <QWidgetAction>

#include <algorithm>
#include <utility>

namespace javelin::gui::shell
{
    namespace
    {
        constexpr qsizetype MaxVisibleSearchResults = 50;

        class MessageTransferDestinationMenuFilter final : public QObject
        {
          public:
            MessageTransferDestinationMenuFilter(
                QMenu& menu, QWidgetAction& searchAction,
                MessageTransferDestinationPresentation presentation,
                MessageTransferDestinationTriggered triggered)
                : QObject(&searchAction), m_menu(menu), m_searchAction(searchAction),
                  m_presentation(std::move(presentation)), m_triggered(std::move(triggered))
            {
                auto* search = new QLineEdit(&menu);
                search->setObjectName(QStringLiteral("messageTransferDestinationSearch"));
                search->setPlaceholderText(i18n("Search mailboxes…"));
                search->setClearButtonEnabled(true);
                search->setMinimumWidth(260);
                search->setAccessibleName(i18n("Search destination mailboxes"));
                m_search = search;
                m_searchAction.setDefaultWidget(search);
                m_menu.addAction(&m_searchAction);

                m_menu.installEventFilter(this);
                search->installEventFilter(this);
                connect(search, &QLineEdit::textChanged, this,
                        [this](const QString& text) { rebuild(text); });
                rebuild({});
            }

          protected:
            bool eventFilter(QObject* watched, QEvent* event) override
            {
                if (event->type() != QEvent::KeyPress)
                    return QObject::eventFilter(watched, event);

                auto* keyEvent = static_cast<QKeyEvent*>(event);
                if (watched == m_search)
                    return filterSearchKey(*keyEvent);

                if (qobject_cast<QMenu*>(watched) != nullptr && printableSearchKey(*keyEvent))
                {
                    if (watched != &m_menu)
                        qobject_cast<QMenu*>(watched)->hide();
                    m_search->setFocus(Qt::ShortcutFocusReason);
                    m_search->insert(keyEvent->text());
                    return true;
                }
                return QObject::eventFilter(watched, event);
            }

          private:
            [[nodiscard]] static bool printableSearchKey(const QKeyEvent& event)
            {
                if (event.modifiers().testFlag(Qt::ControlModifier) ||
                    event.modifiers().testFlag(Qt::AltModifier) ||
                    event.modifiers().testFlag(Qt::MetaModifier) || event.text().isEmpty())
                    return false;
                return std::ranges::all_of(event.text(), [](const QChar character)
                                           { return character.isPrint(); });
            }

            bool filterSearchKey(QKeyEvent& event)
            {
                if (event.key() == Qt::Key_Escape)
                {
                    m_menu.close();
                    return true;
                }

                if (m_search->text().isEmpty())
                {
                    if (event.key() == Qt::Key_Down || event.key() == Qt::Key_Up)
                    {
                        const auto actions = topLevelDestinationActions();
                        if (actions.isEmpty())
                            return true;
                        m_search->clearFocus();
                        m_menu.setActiveAction(event.key() == Qt::Key_Down ? actions.front()
                                                                           : actions.back());
                        return true;
                    }
                    return QObject::eventFilter(m_search, &event);
                }

                if (event.key() == Qt::Key_Down || event.key() == Qt::Key_Up)
                {
                    selectAdjacentResult(event.key() == Qt::Key_Down ? 1 : -1);
                    return true;
                }
                if (event.key() == Qt::Key_Return || event.key() == Qt::Key_Enter)
                {
                    auto* action = m_menu.activeAction();
                    if (!m_resultActions.contains(action) && !m_resultActions.isEmpty())
                        action = m_resultActions.front();
                    if (action != nullptr && action->isEnabled())
                    {
                        action->trigger();
                        m_menu.close();
                    }
                    return true;
                }
                return QObject::eventFilter(m_search, &event);
            }

            void selectAdjacentResult(const int step)
            {
                if (m_resultActions.isEmpty())
                    return;
                const auto* active = m_menu.activeAction();
                auto index = m_resultActions.indexOf(const_cast<QAction*>(active));
                if (index < 0)
                    index = step > 0 ? -1 : 0;
                index = (index + step + m_resultActions.size()) % m_resultActions.size();
                m_menu.setActiveAction(m_resultActions.at(index));
            }

            [[nodiscard]] QList<QAction*> topLevelDestinationActions() const
            {
                QList<QAction*> result;
                for (auto* action : m_contentActions)
                {
                    if (action != nullptr && !action->isSeparator() && action->isEnabled())
                        result.push_back(action);
                }
                for (auto* submenu : m_submenus)
                {
                    if (submenu != nullptr && submenu->menuAction()->isEnabled())
                        result.push_back(submenu->menuAction());
                }
                return result;
            }

            void clearContent()
            {
                m_menu.setActiveAction(nullptr);
                m_resultActions.clear();
                for (auto* submenu : std::as_const(m_submenus))
                    delete submenu;
                m_submenus.clear();
                for (auto* action : std::as_const(m_contentActions))
                    delete action;
                m_contentActions.clear();
            }

            QAction* addSeparator(QMenu& menu)
            {
                auto* action = menu.addSeparator();
                if (&menu == &m_menu)
                    m_contentActions.push_back(action);
                return action;
            }

            QAction* addDestinationAction(QMenu& menu, const MessageTransferDestinationRow& row,
                                          const QString& label)
            {
                const auto iconColor = menu.palette().color(QPalette::Active, QPalette::Text);
                auto* action = menu.addAction(
                    javelin::gui::mailboxes::mailboxPresentationIcon(row.role, iconColor), label);
                action->setProperty("javelinDestinationAccountId",
                                    QString::fromStdString(row.accountId));
                action->setProperty("javelinDestinationMailboxId",
                                    QString::fromStdString(row.mailboxId));
                if (&menu == &m_menu)
                    m_contentActions.push_back(action);
                connect(action, &QAction::triggered, this,
                        [this, row]
                        {
                            if (m_triggered)
                                m_triggered(row);
                        });
                return action;
            }

            void addRows(QMenu& menu, const std::vector<MessageTransferDestinationRow>& rows)
            {
                for (const auto& row : rows)
                {
                    if (row.separatorBefore && !menu.actions().empty())
                    {
                        if (&menu == &m_menu)
                            addSeparator(menu);
                        else
                            menu.addSeparator();
                    }
                    const QString indentation(static_cast<qsizetype>(row.depth), QChar{u'\u2003'});
                    addDestinationAction(menu, row,
                                         indentation + QString::fromStdString(row.mailboxName));
                }
            }

            void rebuildHierarchy()
            {
                addRows(m_menu, m_presentation.currentAccountRows);
                if (!m_presentation.otherAccounts.empty() &&
                    !m_presentation.currentAccountRows.empty())
                    addSeparator(m_menu);
                for (const auto& account : m_presentation.otherAccounts)
                {
                    auto* submenu = new QMenu(account.label, &m_menu);
                    submenu->installEventFilter(this);
                    m_menu.addMenu(submenu);
                    m_submenus.push_back(submenu);
                    addRows(*submenu, account.rows);
                }
            }

            void rebuildSearch(const QString& text)
            {
                const auto results = searchMessageTransferDestinations(m_presentation, text);
                const auto visibleCount =
                    std::min(static_cast<qsizetype>(results.size()), MaxVisibleSearchResults);
                for (qsizetype index = 0; index < visibleCount; ++index)
                {
                    const auto& row = results.at(static_cast<std::size_t>(index)).destination;
                    const auto label = i18nc("mailbox path and account in a transfer search result",
                                             "%1 — %2", row.mailboxPath, row.accountLabel);
                    m_resultActions.push_back(addDestinationAction(m_menu, row, label));
                }

                if (results.empty())
                {
                    auto* empty = m_menu.addAction(i18n("No matching mailboxes"));
                    empty->setEnabled(false);
                    m_contentActions.push_back(empty);
                }
                else if (results.size() > static_cast<std::size_t>(MaxVisibleSearchResults))
                {
                    auto* more =
                        m_menu.addAction(i18n("More matches — keep typing to narrow results"));
                    more->setEnabled(false);
                    m_contentActions.push_back(more);
                }

                if (!m_resultActions.isEmpty())
                    m_menu.setActiveAction(m_resultActions.front());
            }

            void rebuild(const QString& searchText)
            {
                clearContent();
                addSeparator(m_menu);
                if (searchText.trimmed().isEmpty())
                    rebuildHierarchy();
                else
                    rebuildSearch(searchText);
            }

            QMenu& m_menu;
            QWidgetAction& m_searchAction;
            QPointer<QLineEdit> m_search;
            MessageTransferDestinationPresentation m_presentation;
            MessageTransferDestinationTriggered m_triggered;
            QList<QAction*> m_contentActions;
            QList<QAction*> m_resultActions;
            QList<QMenu*> m_submenus;
        };
    } // namespace

    bool populateMessageTransferDestinationMenu(
        QMenu& menu, const MessageTransferDestinationPresentation& presentation,
        MessageTransferDestinationTriggered triggered)
    {
        const bool hasDestinations =
            !presentation.currentAccountRows.empty() || !presentation.otherAccounts.empty();
        if (!hasDestinations)
            return false;

        auto* searchAction = new QWidgetAction(&menu);
        searchAction->setObjectName(QStringLiteral("messageTransferDestinationSearchAction"));
        static_cast<void>(new MessageTransferDestinationMenuFilter(
            menu, *searchAction, presentation, std::move(triggered)));
        return true;
    }

} // namespace javelin::gui::shell
