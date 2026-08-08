#include "gui/messages/MessageListPanePresenter.h"

#include "gui/shell/ElidingLabel.h"

#include <KLocalizedString>

#include <QLabel>
#include <QListView>
#include <QProgressBar>
#include <QToolButton>
#include <QWidget>

namespace javelin::gui::messages
{
    MessageListPanePresenter::MessageListPanePresenter(
        javelin::gui::shell::ElidingLabel& titleLabel, QLabel& metaLabel, QLabel& emptyState,
        QListView& messageView, QProgressBar& loadingIndicator, QToolButton& searchServerButton,
        QWidget& continuationFooter, QLabel& continuationLabel,
        QToolButton& continuationRetryButton)
        : m_titleLabel(titleLabel), m_metaLabel(metaLabel), m_emptyState(emptyState),
          m_messageView(messageView), m_loadingIndicator(loadingIndicator),
          m_searchServerButton(searchServerButton), m_continuationFooter(continuationFooter),
          m_continuationLabel(continuationLabel), m_continuationRetryButton(continuationRetryButton)
    {
    }

    void MessageListPanePresenter::showEmptyState(const MessageListEmptyState& state) const
    {
        if (!state.refreshError.isEmpty())
        {
            m_emptyState.setText(
                i18n("Could not refresh the message list.\n%1", state.refreshError));
            m_emptyState.setStyleSheet(QStringLiteral("color: #e58b8b;"));
        }
        else if (state.refreshInFlight && state.itemCount == 0)
        {
            m_emptyState.setText(i18n("Looking for messages…"));
            m_emptyState.setStyleSheet(QString{});
        }
        else if (state.collection == MessageCollectionKind::LocalSearch)
        {
            m_emptyState.setText(i18n("No indexed messages on this device matched your search."));
            m_emptyState.setStyleSheet(QString{});
        }
        else if (state.collection == MessageCollectionKind::OnlineSearch)
        {
            m_emptyState.setText(i18n("No messages matched your search."));
            m_emptyState.setStyleSheet(QString{});
        }
        else
        {
            m_emptyState.setText(i18n("No messages"));
            m_emptyState.setStyleSheet(QString{});
        }
        m_emptyState.setVisible(state.itemCount == 0);
        m_messageView.setVisible(true);
    }

    void MessageListPanePresenter::showNoContext() const
    {
        m_titleLabel.setText(i18n("Messages"));
        m_metaLabel.clear();
        m_searchServerButton.setVisible(false);
        showLoadingIndicator(false);
        hideContinuation();
    }

    void MessageListPanePresenter::showContext(const MessageListContextHeader& header) const
    {
        m_titleLabel.setText(header.title);
        m_metaLabel.setText(header.context);
        m_searchServerButton.setVisible(false);
        showLoadingIndicator(false);
        hideContinuation();
    }

    void MessageListPanePresenter::showList(const MessageListHeader& header) const
    {
        m_titleLabel.setText(header.title);
        m_searchServerButton.setVisible(header.canSearchServer);
        showLoadingIndicator(header.refreshInFlight);

        if (header.total.has_value())
        {
            m_metaLabel.setText(header.indexedSearch
                                    ? i18np("%1 Indexed Match", "%1 Indexed Matches", *header.total)
                                : header.search
                                    ? i18np("%1 Match", "%1 Matches", *header.total)
                                    : i18np("%1 Conversation", "%1 Conversations", *header.total));
        }
        else
        {
            m_metaLabel.setText(
                header.search
                    ? i18np("%1 Loaded Match", "%1 Loaded Matches", header.itemCount)
                    : i18np("%1 Loaded Conversation", "%1 Loaded Conversations", header.itemCount));
        }

        if (!header.loadMoreError.isEmpty())
        {
            m_continuationLabel.setText(
                i18n("Could not load more messages. %1", header.loadMoreError));
            m_continuationRetryButton.setVisible(true);
            m_continuationFooter.setVisible(true);
        }
        else if (header.loadMoreInFlight)
        {
            m_continuationLabel.setText(i18n("Loading more messages…"));
            m_continuationRetryButton.setVisible(false);
            m_continuationFooter.setVisible(true);
        }
        else
        {
            hideContinuation();
        }
    }

    void MessageListPanePresenter::showLoadingIndicator(const bool inFlight) const
    {
        if (inFlight)
        {
            m_loadingIndicator.setRange(0, 0);
            return;
        }
        m_loadingIndicator.setRange(0, 1);
        m_loadingIndicator.setValue(0);
    }

    void MessageListPanePresenter::hideContinuation() const
    {
        m_continuationFooter.setVisible(false);
        m_continuationRetryButton.setVisible(false);
        m_continuationLabel.clear();
    }
} // namespace javelin::gui::messages
