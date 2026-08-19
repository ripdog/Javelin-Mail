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
        javelin::gui::shell::ElidingLabel& titleLabel, QLabel& metaLabel, QWidget& emptyStatePanel,
        QLabel& emptyState, QToolButton& emptyStateAction, QListView& messageView,
        QProgressBar& loadingIndicator, QToolButton& searchServerButton,
        QWidget& continuationFooter, QLabel& continuationLabel,
        QToolButton& continuationRetryButton)
        : m_titleLabel(titleLabel), m_metaLabel(metaLabel), m_emptyStatePanel(emptyStatePanel),
          m_emptyState(emptyState), m_emptyStateAction(emptyStateAction),
          m_messageView(messageView), m_loadingIndicator(loadingIndicator),
          m_searchServerButton(searchServerButton), m_continuationFooter(continuationFooter),
          m_continuationLabel(continuationLabel), m_continuationRetryButton(continuationRetryButton)
    {
    }

    void MessageListPanePresenter::showEmptyState(const MessageListEmptyState& state) const
    {
        QString text;
        QString actionText;
        switch (state.kind)
        {
        case MessageListEmptyStateKind::EmptyMailbox:
            text = i18n("This mailbox is empty.");
            break;
        case MessageListEmptyStateKind::NoFilterMatches:
            text = i18n("No messages match the current filters.");
            actionText = i18nc("@action:button", "Clear Filters");
            break;
        case MessageListEmptyStateKind::NoLocalSearchResults:
            text = i18n("No indexed messages on this device matched your search.");
            if (state.action == MessageListEmptyAction::SearchServer)
                actionText = i18nc("@action:button", "Search Server");
            else if (state.action == MessageListEmptyAction::EditSearch)
                actionText = i18nc("@action:button", "Edit Search");
            break;
        case MessageListEmptyStateKind::NoSearchResults:
            text = i18n("No messages matched your search.");
            actionText = i18nc("@action:button", "Edit Search");
            break;
        case MessageListEmptyStateKind::Disconnected:
            text = i18n("Javelin is offline. No cached messages are available for this view.");
            actionText = i18nc("@action:button", "Retry");
            break;
        case MessageListEmptyStateKind::Connecting:
            text = i18n("Connecting to the mail server…");
            break;
        case MessageListEmptyStateKind::AuthenticationRequired:
            text = i18n("Sign in again to load messages for this account.");
            actionText = i18nc("@action:button", "Sign In Again");
            break;
        case MessageListEmptyStateKind::RefreshFailed:
            text = state.detail.isEmpty()
                       ? i18n("Could not load the message list.")
                       : i18n("Could not load the message list.\n%1", state.detail);
            actionText = i18nc("@action:button", "Retry");
            break;
        case MessageListEmptyStateKind::NotYetLoaded:
            text = i18n("Messages have not been loaded for this view yet.");
            actionText = i18nc("@action:button", "Retry");
            break;
        case MessageListEmptyStateKind::Loading:
            text = i18n("Looking for messages…");
            break;
        }

        m_emptyState.setText(text);
        m_emptyStateAction.setText(actionText);
        m_emptyStateAction.setVisible(!actionText.isEmpty());
        const bool empty = state.itemCount == 0;
        m_emptyStatePanel.setVisible(empty);
        m_messageView.setVisible(!empty);
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
