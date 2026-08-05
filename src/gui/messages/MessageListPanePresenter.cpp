#include "gui/messages/MessageListPanePresenter.h"

#include "gui/messages/Pagination.h"
#include "gui/shell/ElidingLabel.h"

#include <KLocalizedString>

#include <QLabel>
#include <QListView>
#include <QProgressBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>

#include <algorithm>
#include <limits>

namespace javelin::gui::messages
{
    MessageListPanePresenter::MessageListPanePresenter(
        javelin::gui::shell::ElidingLabel& titleLabel, QLabel& metaLabel, QLabel& pageLabel,
        QLabel& emptyState, QListView& messageView, QProgressBar& loadingIndicator,
        QToolButton& searchServerButton, QToolButton& firstPageButton,
        QToolButton& previousPageButton, QSpinBox& pageNumberSpinBox, QToolButton& nextPageButton,
        QToolButton& lastPageButton, const std::size_t defaultPageSize)
        : m_titleLabel(titleLabel), m_metaLabel(metaLabel), m_pageLabel(pageLabel),
          m_emptyState(emptyState), m_messageView(messageView),
          m_loadingIndicator(loadingIndicator), m_searchServerButton(searchServerButton),
          m_firstPageButton(firstPageButton), m_previousPageButton(previousPageButton),
          m_pageNumberSpinBox(pageNumberSpinBox), m_nextPageButton(nextPageButton),
          m_lastPageButton(lastPageButton), m_defaultPageSize(defaultPageSize)
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
            m_emptyState.setText(
                i18n("No indexed messages on this device matched your search."));
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
        disablePagination();
    }

    void MessageListPanePresenter::showContext(const MessageListContextHeader& header) const
    {
        m_titleLabel.setText(header.title);
        m_metaLabel.setText(header.context);
        m_searchServerButton.setVisible(false);
        showLoadingIndicator(false);
        disablePagination();
    }

    void MessageListPanePresenter::showPage(const MessageListPageHeader& header) const
    {
        m_titleLabel.setText(header.title);
        m_searchServerButton.setVisible(header.canSearchServer);
        showLoadingIndicator(header.refreshInFlight);
        const auto step = header.returnedLimit == 0 ? m_defaultPageSize : header.returnedLimit;
        const QSignalBlocker pageNumberBlocker{&m_pageNumberSpinBox};
        if (!header.total)
        {
            m_metaLabel.setText(
                header.search
                    ? i18np("%1 Loaded Match", "%1 Loaded Matches", header.itemCount)
                    : i18np("%1 Loaded Conversation", "%1 Loaded Conversations",
                            header.itemCount));
            m_pageLabel.clear();
            m_firstPageButton.setEnabled(header.offset > 0);
            m_previousPageButton.setEnabled(header.offset > 0);
            m_pageNumberSpinBox.setRange(0, 0);
            m_pageNumberSpinBox.setValue(0);
            m_pageNumberSpinBox.setSuffix(QString{});
            m_pageNumberSpinBox.setEnabled(false);
            m_nextPageButton.setEnabled(false);
            m_lastPageButton.setEnabled(false);
            return;
        }

        m_metaLabel.setText(
            header.indexedSearch
                ? i18np("%1 Indexed Match", "%1 Indexed Matches", *header.total)
            : header.search
                ? i18np("%1 Match", "%1 Matches", *header.total)
                : i18np("%1 Conversation", "%1 Conversations", *header.total));
        const auto metrics = pageMetrics(header.position, header.itemCount, *header.total);
        m_pageLabel.setText(*header.total == 0 ? QStringLiteral("0-0")
                                               : QStringLiteral("%1-%2")
                                                     .arg(static_cast<qulonglong>(metrics.start))
                                                     .arg(static_cast<qulonglong>(metrics.end)));
        const auto pages = pageCount(*header.total, step);
        const auto currentPage =
            pages == 0 ? std::size_t{0} : std::min(pageIndex(header.position, step) + 1, pages);
        const auto intMaximum = static_cast<std::size_t>(std::numeric_limits<int>::max());
        m_pageNumberSpinBox.setRange(pages == 0 ? 0 : 1,
                                     static_cast<int>(std::min(pages, intMaximum)));
        m_pageNumberSpinBox.setValue(static_cast<int>(std::min(currentPage, intMaximum)));
        m_pageNumberSpinBox.setSuffix(
            i18nc("@info page number suffix", " of %1", static_cast<qulonglong>(pages)));
        m_pageNumberSpinBox.setEnabled(pages > 0);
        m_firstPageButton.setEnabled(header.position > 0);
        m_previousPageButton.setEnabled(header.position > 0);
        m_nextPageButton.setEnabled(metrics.hasNext);
        m_lastPageButton.setEnabled(metrics.hasNext);
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

    void MessageListPanePresenter::disablePagination() const
    {
        m_pageLabel.clear();
        m_firstPageButton.setEnabled(false);
        m_previousPageButton.setEnabled(false);
        m_pageNumberSpinBox.setEnabled(false);
        m_nextPageButton.setEnabled(false);
        m_lastPageButton.setEnabled(false);
    }
} // namespace javelin::gui::messages
