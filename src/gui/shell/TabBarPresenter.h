#pragma once

#include "gui/shell/TabWorkspace.h"

#include <QObject>

#include <optional>
#include <vector>

class QIcon;
class QTabBar;
class QWidget;

namespace javelin::jmap::cache
{
    class AccountReader;
    class QueryService;
} // namespace javelin::jmap::cache

namespace javelin::gui::shell
{

    class TabBarPresenter final : public QObject
    {
        Q_OBJECT

      public:
        TabBarPresenter(QTabBar& tabBar, QWidget& window,
                        javelin::jmap::cache::AccountReader& accountReader,
                        javelin::jmap::cache::QueryService& queryService,
                        QObject* parent = nullptr);

        void refresh(const std::vector<TabState>& tabs, std::optional<int> activeIndex);
        [[nodiscard]] QString mailboxTitle(const MailboxTabState& tab) const;

      Q_SIGNALS:
        void closeRequested(int index);

      private:
        [[nodiscard]] QString titleForTab(const TabState& tab) const;
        [[nodiscard]] QIcon iconForTab(const TabState& tab) const;
        void ensureCloseButton(const std::vector<TabState>& tabs, int index);

        QTabBar& m_tabBar;
        QWidget& m_window;
        javelin::jmap::cache::AccountReader& m_accountReader;
        javelin::jmap::cache::QueryService& m_queryService;
    };

} // namespace javelin::gui::shell
