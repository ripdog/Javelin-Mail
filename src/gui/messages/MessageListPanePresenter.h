#pragma once

#include <QString>

#include <cstddef>
#include <optional>

class QLabel;
class QListView;
class QSpinBox;
class QToolButton;

namespace javelin::gui::shell
{
    class ElidingLabel;
}

namespace javelin::gui::messages
{
    enum class MessageCollectionKind
    {
        Mailbox,
        LocalSearch,
        OnlineSearch,
    };

    struct MessageListEmptyState
    {
        std::size_t itemCount = 0;
        QString refreshError;
        bool refreshInFlight = false;
        MessageCollectionKind collection = MessageCollectionKind::Mailbox;
    };

    struct MessageListContextHeader
    {
        QString title;
        QString context;
    };

    struct MessageListPageHeader
    {
        QString title;
        std::size_t offset = 0;
        std::size_t position = 0;
        std::size_t itemCount = 0;
        std::size_t returnedLimit = 0;
        std::optional<std::size_t> total;
        bool search = false;
        bool indexedSearch = false;
        bool canSearchServer = false;
    };

    class MessageListPanePresenter
    {
      public:
        MessageListPanePresenter(javelin::gui::shell::ElidingLabel& titleLabel, QLabel& metaLabel,
                                 QLabel& pageLabel, QLabel& emptyState, QListView& messageView,
                                 QToolButton& searchServerButton, QToolButton& firstPageButton,
                                 QToolButton& previousPageButton, QSpinBox& pageNumberSpinBox,
                                 QToolButton& nextPageButton, QToolButton& lastPageButton,
                                 std::size_t defaultPageSize);

        void showEmptyState(const MessageListEmptyState& state) const;
        void showNoContext() const;
        void showContext(const MessageListContextHeader& header) const;
        void showPage(const MessageListPageHeader& header) const;

      private:
        void disablePagination() const;

        javelin::gui::shell::ElidingLabel& m_titleLabel;
        QLabel& m_metaLabel;
        QLabel& m_pageLabel;
        QLabel& m_emptyState;
        QListView& m_messageView;
        QToolButton& m_searchServerButton;
        QToolButton& m_firstPageButton;
        QToolButton& m_previousPageButton;
        QSpinBox& m_pageNumberSpinBox;
        QToolButton& m_nextPageButton;
        QToolButton& m_lastPageButton;
        std::size_t m_defaultPageSize;
    };
} // namespace javelin::gui::messages
