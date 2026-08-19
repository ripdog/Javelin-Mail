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
    enum class MessageListEmptyStateKind
    {
        EmptyMailbox,
        NoFilterMatches,
        NoLocalSearchResults,
        NoSearchResults,
        Disconnected,
        Connecting,
        AuthenticationRequired,
        RefreshFailed,
        NotYetLoaded,
        Loading,
    };

    enum class MessageListEmptyAction
    {
        None,
        ClearFilters,
        SearchServer,
        EditSearch,
        Retry,
        SignInAgain,
    };

    struct MessageListEmptyState
    {
        std::size_t itemCount = 0;
        MessageListEmptyStateKind kind = MessageListEmptyStateKind::EmptyMailbox;
        MessageListEmptyAction action = MessageListEmptyAction::None;
        QString detail;
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
                                 QWidget& emptyStatePanel, QLabel& emptyState,
                                 QToolButton& emptyStateAction, QListView& messageView,
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
        QWidget& m_emptyStatePanel;
        QLabel& m_emptyState;
        QToolButton& m_emptyStateAction;
        QListView& m_messageView;
        QProgressBar& m_loadingIndicator;
        QToolButton& m_searchServerButton;
        QWidget& m_continuationFooter;
        QLabel& m_continuationLabel;
        QToolButton& m_continuationRetryButton;
    };
} // namespace javelin::gui::messages
