#pragma once

#include "gui/shell/TabWorkspace.h"
#include "jmap/search/EmailSearch.h"

#include <QObject>

#include <optional>
#include <string>
#include <utility>
#include <vector>

class QColor;
class QLineEdit;
class QMenu;
class QToolButton;
class QWidget;

namespace javelin::jmap::cache
{
    class MailTagReader;
}

namespace javelin::gui::shell
{
    struct QuickFilterWidgets
    {
        QToolButton& toggleButton;
        QWidget& panel;
        QToolButton& pinButton;
        QToolButton& unreadButton;
        QToolButton& starredButton;
        QToolButton& contactButton;
        QToolButton& tagsButton;
        QToolButton& attachmentButton;
        QLineEdit& textEdit;
        QToolButton& senderButton;
        QToolButton& recipientsButton;
        QToolButton& subjectButton;
        QToolButton& bodyButton;
        QMenu& tagsMenu;
    };

    class QuickFilterController final : public QObject
    {
        Q_OBJECT

      public:
        QuickFilterController(javelin::jmap::cache::MailTagReader& mailTagReader,
                              QuickFilterWidgets widgets, QWidget& shortcutParent,
                              QObject* parent = nullptr);

        void activate(TabState* tab);
        void clear();
        void syncContinuitySelection(std::optional<std::string> emailId,
                                     std::optional<std::string> threadId);
        void rebuildTagsMenu();
        void removeTag(std::string_view keyword);
        void updateIcons(const QColor& iconColor);

      private:
        [[nodiscard]] MailboxTabState* activeMailbox() const;
        [[nodiscard]] javelin::jmap::search::EmailSearchCriteria criteriaFromUi() const;
        void apply();
        void syncUiFromSession();

        javelin::jmap::cache::MailTagReader& m_mailTagReader;
        QuickFilterWidgets m_widgets;
        TabState* m_activeTab = nullptr;
        std::vector<std::string> m_tags;
        bool m_matchAllTags = false;
        bool m_pinned = false;
        javelin::jmap::search::EmailSearchCriteria m_pinnedCriteria;
        std::optional<std::pair<std::string, std::string>> m_lastMailbox;
    };
} // namespace javelin::gui::shell
