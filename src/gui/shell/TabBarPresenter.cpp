#include "gui/shell/TabBarPresenter.h"

#include "app/MailboxSession.h"
#include "app/SearchSession.h"
#include "gui/IconUtils.h"
#include "gui/mailboxes/MailboxIconUtils.h"
#include "gui/settings/GuiSettings.h"

#include <KLocalizedString>

#include <QIcon>
#include <QPalette>
#include <QSignalBlocker>
#include <QTabBar>
#include <QToolButton>
#include <QWidget>

#include <variant>

namespace javelin::gui::shell
{

    TabBarPresenter::TabBarPresenter(javelin::gui::settings::GuiSettings& settings, QTabBar& tabBar,
                                     QWidget& window, QObject* parent)
        : QObject(parent), m_settings(settings), m_tabBar(tabBar), m_window(window)
    {
    }

    void TabBarPresenter::refresh(const std::vector<TabState>& tabs,
                                  const std::optional<int> activeIndex)
    {
        QSignalBlocker blocker{&m_tabBar};
        if (m_tabBar.count() != static_cast<int>(tabs.size()))
        {
            while (m_tabBar.count() > 0)
            {
                m_tabBar.removeTab(0);
            }
            for (const auto& tab : tabs)
            {
                m_tabBar.addTab(iconForTab(tab), titleForTab(tab));
            }
        }
        else
        {
            for (int index = 0; index < static_cast<int>(tabs.size()); ++index)
            {
                const auto& tab = tabs[static_cast<std::size_t>(index)];
                const auto title = titleForTab(tab);
                if (m_tabBar.tabText(index) != title)
                {
                    m_tabBar.setTabText(index, title);
                }
                m_tabBar.setTabIcon(index, iconForTab(tab));
            }
        }

        for (int index = 0; index < static_cast<int>(tabs.size()); ++index)
        {
            ensureCloseButton(tabs, index);
        }

        if (activeIndex.has_value() && *activeIndex >= 0 && *activeIndex < m_tabBar.count() &&
            m_tabBar.currentIndex() != *activeIndex)
        {
            m_tabBar.setCurrentIndex(*activeIndex);
        }
        m_tabBar.setVisible(tabs.size() > 1);

        const auto* active = activeWorkspaceTab(tabs, activeIndex);
        m_window.setWindowTitle(active == nullptr ? i18n("Javelin Mail") : titleForTab(*active));
    }

    QString TabBarPresenter::mailboxTitle(const MailboxTabState& tab) const
    {
        if (tab.session == nullptr)
        {
            return {};
        }
        return tab.session->title();
    }

    QString TabBarPresenter::titleForTab(const TabState& tab) const
    {
        QString title;
        const auto accountId = tabAccountId(tab).value_or(std::string{});
        if (const auto* mailboxTab = std::get_if<MailboxTabState>(&tab.content))
        {
            title = mailboxTitle(*mailboxTab);
        }
        else if (const auto* searchTab = std::get_if<SearchTabState>(&tab.content))
        {
            title = searchTab->session == nullptr ? QString{} : searchTab->session->title();
        }
        else if (const auto* contactsTab = std::get_if<ContactsTabState>(&tab.content))
        {
            title = contactsTab->title;
        }
        else if (const auto* calendarTab = std::get_if<CalendarTabState>(&tab.content))
        {
            title = calendarTab->title;
        }
        else
        {
            title = std::get<ComposeTabState>(tab.content).title;
        }

        const auto settings = m_settings.accountForCachedId(QString::fromStdString(accountId));
        auto accountName = settings.displayName;
        if (accountName.isEmpty())
        {
            accountName = settings.loginEmail;
        }
        return accountName.isEmpty() ? title
                                     : i18nc("@title:window tab title and account name", "%1 - %2",
                                             title, accountName);
    }

    QIcon TabBarPresenter::iconForTab(const TabState& tab) const
    {
        const auto color = m_tabBar.palette().color(QPalette::Active, QPalette::Text);
        if (const auto* mailboxTab = std::get_if<MailboxTabState>(&tab.content))
        {
            return mailboxTab->session == nullptr
                       ? QIcon{}
                       : javelin::gui::mailboxes::mailboxIcon(mailboxTab->session->role(), color);
        }
        if (std::holds_alternative<SearchTabState>(tab.content))
        {
            return javelin::gui::themedSvgIcon(
                QStringLiteral(":/icons/thunderbird-icons/search.svg"), color);
        }
        if (std::holds_alternative<ContactsTabState>(tab.content))
        {
            return QIcon::fromTheme(QStringLiteral("view-pim-contacts"));
        }
        if (std::holds_alternative<CalendarTabState>(tab.content))
        {
            return QIcon::fromTheme(QStringLiteral("view-calendar-month"));
        }
        return javelin::gui::themedSvgIcon(QStringLiteral(":/icons/thunderbird-icons/new-mail.svg"),
                                           color);
    }

    void TabBarPresenter::ensureCloseButton(const std::vector<TabState>& tabs, const int index)
    {
        const bool canClose =
            tabCanClose(tabs[static_cast<std::size_t>(index)], static_cast<std::size_t>(index));
        if (!canClose)
        {
            m_tabBar.setTabButton(index, QTabBar::RightSide, nullptr);
            return;
        }
        if (m_tabBar.tabButton(index, QTabBar::RightSide) != nullptr)
        {
            return;
        }

        auto* closeButton = new QToolButton(&m_tabBar);
        closeButton->setAutoRaise(true);
        closeButton->setText(QStringLiteral("x"));
        closeButton->setAccessibleName(i18nc("@action:button", "Close tab"));
        closeButton->setToolTip(i18nc("@info:tooltip", "Close tab"));
        connect(closeButton, &QToolButton::clicked, this,
                [this, index] { Q_EMIT closeRequested(index); });
        m_tabBar.setTabButton(index, QTabBar::RightSide, closeButton);
    }

} // namespace javelin::gui::shell
