#pragma once

#include <QString>

#include <cstddef>
#include <optional>

class QLabel;
class QListView;
class QProgressBar;
class QToolButton;
class QWidget;

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

    struct MessageListHeader
    {
        QString title;
        std::size_t itemCount = 0;
        std::optional<std::size_t> total;
        bool search = false;
        bool indexedSearch = false;
        bool canSearchServer = false;
        bool refreshInFlight = false;
        bool loadMoreInFlight = false;
        QString loadMoreError;
    };

    class MessageListPanePresenter
    {
      public:
        MessageListPanePresenter(javelin::gui::shell::ElidingLabel& titleLabel, QLabel& metaLabel,
                                 QLabel& emptyState, QListView& messageView,
                                 QProgressBar& loadingIndicator, QToolButton& searchServerButton,
                                 QWidget& continuationFooter, QLabel& continuationLabel,
                                 QToolButton& continuationRetryButton);

        void showEmptyState(const MessageListEmptyState& state) const;
        void showNoContext() const;
        void showContext(const MessageListContextHeader& header) const;
        void showList(const MessageListHeader& header) const;

      private:
        void showLoadingIndicator(bool inFlight) const;
        void hideContinuation() const;

        javelin::gui::shell::ElidingLabel& m_titleLabel;
        QLabel& m_metaLabel;
        QLabel& m_emptyState;
        QListView& m_messageView;
        QProgressBar& m_loadingIndicator;
        QToolButton& m_searchServerButton;
        QWidget& m_continuationFooter;
        QLabel& m_continuationLabel;
        QToolButton& m_continuationRetryButton;
    };
} // namespace javelin::gui::messages
