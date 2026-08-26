#include "gui/messages/MessageDragListView.h"

#include "gui/messages/MessageListModel.h"

#include <KLocalizedString>

#include <QApplication>
#include <QCursor>
#include <QDrag>
#include <QFileInfo>
#include <QFocusEvent>
#include <QItemSelectionModel>
#include <QMimeData>
#include <QPainter>

#include <algorithm>
#include <memory>
#include <ranges>
#include <utility>

namespace javelin::gui::messages
{
    QMimeData* buildMessageDragMimeData(const QByteArray& internalPayload,
                                        const QList<QUrl>& externalUrls,
                                        const bool advertiseExternalFiles)
    {
        if (internalPayload.isEmpty())
            return nullptr;
        auto* mimeData = new QMimeData;
        mimeData->setData(QString::fromLatin1(messageDragMimeType), internalPayload);
        if (!externalUrls.isEmpty())
            mimeData->setUrls(externalUrls);
        else if (advertiseExternalFiles)
            mimeData->setData(QStringLiteral("text/uri-list"), {});
        return mimeData;
    }

    qsizetype representedMessageCountForDrag(const QModelIndexList& indexes)
    {
        qsizetype count = 0;
        for (const auto& index : indexes)
        {
            qsizetype represented = 1;
            const auto rowKind = static_cast<MessageListModel::RowKind>(
                index.data(MessageListModel::RowKindRole).toInt());
            const bool collapsedSummary = rowKind == MessageListModel::RowKind::ThreadSummary &&
                                          !index.data(MessageListModel::IsExpandedRole).toBool();
            if (collapsedSummary)
            {
                const auto mailboxCount = index.data(MessageListModel::ThreadMessageCountRole);
                if (mailboxCount.isValid())
                    represented = std::max<qsizetype>(1, mailboxCount.toLongLong());
            }
            count += represented;
        }
        return count;
    }

    void MessageDragListView::focusInEvent(QFocusEvent* event)
    {
        QListView::focusInEvent(event);
        auto* selection = selectionModel();
        if (selection == nullptr || !currentIndex().isValid() ||
            !selection->selectedRows().isEmpty())
            return;

        selection->select(currentIndex(),
                          QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }

    void MessageDragListView::setExternalFileDragEnabled(const bool enabled)
    {
        m_externalFileDragEnabled = enabled;
        if (!enabled)
        {
            m_activeRequestId.reset();
            m_activeDragMimeData.clear();
            m_requestedPayloads.clear();
            m_cachedPayload.clear();
            m_cachedUrls.clear();
        }
    }

    void MessageDragListView::startDrag(const Qt::DropActions supportedActions)
    {
        auto indexes = selectionModel()->selectedRows();
        if (indexes.isEmpty() && currentIndex().isValid())
            indexes.push_back(currentIndex());

        std::unique_ptr<QMimeData> modelMime{model()->mimeData(indexes)};
        if (modelMime == nullptr)
            return;
        const auto internalPayload = modelMime->data(QString::fromLatin1(messageDragMimeType));
        const auto decoded = decodeMessageDragPayload(internalPayload);
        if (internalPayload.isEmpty() || !decoded.has_value())
            return;

        QList<QUrl> externalUrls;
        const bool cachedFilesAvailable =
            m_externalFileDragEnabled && m_cachedPayload == internalPayload &&
            !m_cachedUrls.isEmpty() &&
            std::ranges::all_of(
                m_cachedUrls, [](const QUrl& url)
                { return url.isLocalFile() && QFileInfo::exists(url.toLocalFile()); });
        if (cachedFilesAvailable)
        {
            externalUrls = std::exchange(m_cachedUrls, {});
            m_cachedPayload.clear();
        }

        // Keep the internal mailbox-transfer payload immediately usable. External file targets
        // need text/uri-list to be offered from drag start, but the raw EML files may require
        // asynchronous daemon materialization. Populate that already-advertised format once the
        // cursor actually leaves this application; if the drag ends first, the completed files are
        // cached for the next identical drag rather than blocking the GUI with a nested event loop.
        auto* dragMimeData = buildMessageDragMimeData(
            internalPayload, externalUrls, m_externalFileDragEnabled && externalUrls.isEmpty());
        if (dragMimeData == nullptr)
            return;

        const auto requestId = m_nextRequestId++;
        bool preparationRequested = false;
        const auto requestPreparation = [this, requestId, internalPayload, payload = *decoded,
                                         dragMimeData, &preparationRequested]
        {
            if (preparationRequested || QApplication::widgetAt(QCursor::pos()) != nullptr)
                return;
            preparationRequested = true;
            m_activeRequestId = requestId;
            m_activeDragMimeData = dragMimeData;
            m_requestedPayloads.insert(requestId, internalPayload);
            Q_EMIT externalDragPreparationRequested(requestId, payload);
        };

        const QString label =
            i18np("%1 message", "%1 messages", representedMessageCountForDrag(indexes));
        const QFontMetrics metrics{font()};
        const QSize badgeSize{metrics.horizontalAdvance(label) + 48,
                              std::max(34, metrics.height() + 14)};
        const qreal scale = devicePixelRatioF();
        QPixmap badge{badgeSize * scale};
        badge.setDevicePixelRatio(scale);
        badge.fill(Qt::transparent);

        QPainter painter{&badge};
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().highlight());
        painter.drawRoundedRect(QRect{QPoint{}, badgeSize}.adjusted(1, 1, -1, -1), 8, 8);

        const QRect envelopeRect{12, (badgeSize.height() - 14) / 2, 20, 14};
        QPen envelopePen{palette().highlightedText(), 1.5};
        painter.setPen(envelopePen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(envelopeRect, 2, 2);
        painter.drawLine(envelopeRect.topLeft(), envelopeRect.center());
        painter.drawLine(envelopeRect.topRight(), envelopeRect.center());

        painter.setPen(palette().highlightedText().color());
        painter.setFont(font());
        painter.drawText(QRect{40, 0, badgeSize.width() - 48, badgeSize.height()},
                         Qt::AlignVCenter | Qt::AlignLeft, label);

        QDrag drag{this};
        drag.setMimeData(dragMimeData);
        drag.setPixmap(badge);
        drag.setHotSpot(QPoint{-10, -10});
        if (m_externalFileDragEnabled && externalUrls.isEmpty())
        {
            connect(&drag, &QDrag::targetChanged, this,
                    [requestPreparation](QObject* target)
                    {
                        if (target == nullptr)
                            requestPreparation();
                    });
            connect(&drag, &QDrag::actionChanged, this,
                    [requestPreparation](Qt::DropAction) { requestPreparation(); });
        }
        static_cast<void>(drag.exec(supportedActions, defaultDropAction()));
        if (m_activeRequestId == std::optional<quint64>{requestId})
        {
            m_activeRequestId.reset();
            m_activeDragMimeData.clear();
        }
    }

    void MessageDragListView::externalDragPreparationReady(const quint64 requestId,
                                                           QList<QUrl> urls)
    {
        const auto found = m_requestedPayloads.find(requestId);
        if (found == m_requestedPayloads.end())
            return;
        const auto payload = found.value();
        m_requestedPayloads.erase(found);

        if (m_activeRequestId == std::optional<quint64>{requestId} &&
            m_activeDragMimeData != nullptr)
        {
            m_activeDragMimeData->setUrls(urls);
            m_activeRequestId.reset();
            m_activeDragMimeData.clear();
            return;
        }

        m_cachedPayload = payload;
        m_cachedUrls = std::move(urls);
    }

    void MessageDragListView::externalDragPreparationFailed(const quint64 requestId)
    {
        m_requestedPayloads.remove(requestId);
        if (m_activeRequestId == std::optional<quint64>{requestId})
        {
            m_activeRequestId.reset();
            m_activeDragMimeData.clear();
        }
    }

} // namespace javelin::gui::messages
